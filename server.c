/* Por Prof. Daniel Batista <batista@ime.usp.br>
 * Em 9/8/2017
 * 
 * Um código simples (não é o código ideal, mas é o suficiente para o
 * EP) de um servidor de eco a ser usado como base para o EP1. Ele
 * recebe uma linha de um cliente e devolve a mesma linha. Teste ele
 * assim depois de compilar:
 * 
 * ./servidor 8000
 * 
 * Com este comando o servidor ficará escutando por conexões na porta
 * 8000 TCP (Se você quiser fazer o servidor escutar em uma porta
 * menor que 1024 você precisa ser root).
 *
 * Depois conecte no servidor via telnet. Rode em outro terminal:
 * 
 * telnet 127.0.0.1 8000
 * 
 * Escreva sequências de caracteres seguidas de ENTER. Você verá que
 * o telnet exibe a mesma linha em seguida. Esta repetição da linha é
 * enviada pelo servidor. O servidor também exibe no terminal onde ele
 * estiver rodando as linhas enviadas pelos clientes.
 * 
 * Obs.: Você pode conectar no servidor remotamente também. Basta saber o
 * endereço IP remoto da máquina onde o servidor está rodando e não
 * pode haver nenhum firewall no meio do caminho bloqueando conexões na
 * porta escolhida.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <time.h>
#include <unistd.h>
#include <ctype.h>
#include <dirent.h>

#define LISTENQ 1
#define MAXDATASIZE 100
#define MAXLINE 4096
#define MAXSTRING 128

/* States do cliente */
#define NOT_AUTHENTICATED 0
#define AUTHENTICATED 1
#define SELECTED 2

/* Flags dos emails - armazenado como vetor de booleans */
#define MAXFLAGS 2
#define SEEN 0
#define DELETED 1

/* Função que constroi uma resposta FLAGS baseado no vetor flags */
char* build_flags_string(char* flags){
  char* flags_string = malloc(sizeof(char) * 17);
  strcpy(flags_string, "ERROR");
  if (flags[SEEN] && !flags[DELETED]) strcpy(flags_string, "\\Seen");
  if (!flags[SEEN] && flags[DELETED]) strcpy(flags_string, "\\Deleted");
  if (flags[SEEN] && flags[DELETED]) strcpy(flags_string, "\\Seen \\Deleted");
  return flags_string;
} 

/* Função que tenta realizar um login e retorna um booleano */
char attempt_login(char* username, char* password){
  /* Abrindo arquivo de usuários */
  FILE* user_file = fopen("server_data/users.txt", "r");
  if (user_file == NULL){
    fprintf(stderr, "Cannot open users.txt file\n");
    exit(-1);
  }

  char result = 0;
  char * line = NULL;
  size_t len = 0;
  ssize_t read;

  while ((read = getline(&line, &len, user_file)) != -1) {
    line[read] = 0;
    char* current_username = strtok(line, "#\r\n");
    char* current_password = strtok(NULL, "#\r\n");
    
    if (strcmp(current_username, username) == 0 && strcmp(current_password, password) == 0){
      result = 1;
      break;
    }
  }

  fclose(user_file);
  if (line)
      free(line);

  return result;
}

/* Struct que armazena as informações de uma mailbox aberta */
typedef struct _mailbox
{
  char path[MAXSTRING];
  int exists;
  int unseen;
  int recent;
  char flags[MAXFLAGS];
  char status;
} mailbox;

/* Função que seleciona uma mailbox, abre, e retorna
 * as informações gerais */
mailbox open_mailbox(char* mailbox_name, char* username){
  /* Inicializando struct mailbox */
  mailbox current_mailbox;
  current_mailbox.exists = current_mailbox.recent = current_mailbox.status = 0;
  current_mailbox.unseen = -1;
  for (int i = 0; i < MAXFLAGS; i++) current_mailbox.flags[i] = 0;

  /* Determinando o caminho para a mailbox (inbox = root) */
  strcpy(current_mailbox.path, "server_data/");
  strcat(current_mailbox.path, username);
  strcat(current_mailbox.path, "/Maildir/");
  if (strcmp(mailbox_name, "inbox") != 0){
    strcat(current_mailbox.path, mailbox_name);
    strcat(current_mailbox.path, "/");
  }

  /* new and cur paths */
  char new_path[MAXSTRING];
  strcpy(new_path, current_mailbox.path);
  strcat(new_path, "new");
  char cur_path[MAXSTRING];
  strcpy(cur_path, current_mailbox.path);
  strcat(cur_path, "cur");

  /* Contando e movendo as mensagens na pasta "new" */
  DIR* mailbox_new_directory = opendir(new_path);
  struct dirent *new_ent;

  /* Verificando se a pasta foi aberta com sucesso */
  if (mailbox_new_directory){
    /* Passeando por todos os arquivos na pasta */
    while ((new_ent = readdir(mailbox_new_directory)) != NULL){
      /* Ignorando outras pastas, inclusive '...' e '..' */
      if (new_ent->d_type == DT_DIR) continue;

      /* Contando mais uma mensagem recente */
      current_mailbox.recent++;

      /* Movendo para a pasta "cur" */
      char source[MAXSTRING], destination[MAXSTRING];
      strcpy(source, new_path);
      strcat(source, "/");
      strcat(source, new_ent->d_name);
      strcpy(destination, cur_path);
      strcat(destination, "/");
      strcat(destination, new_ent->d_name);
      strcat(destination, ":2,");
      rename(source, destination);
    }
    closedir(mailbox_new_directory); /* Fechando ponteiro de pasta aberto */
  } else{
    /* aconteceu um erro, retornando uma struct com sinal de erro */
    current_mailbox.status = -1;
    return current_mailbox;
  }

  /* Contando e enumerando as mensagens na pasta "cur";
   * também memorizando flags já vistas */
  /* Abrindo a pasta 'cur' */
  DIR* mailbox_cur_directory = opendir(cur_path);
  struct dirent *cur_ent;

  /* Verificando se a pasta foi aberta com sucesso */
  if (mailbox_cur_directory){
    /* Passeando por todos os arquivos na pasta */
    while ((cur_ent = readdir(mailbox_cur_directory)) != NULL){
      /* Ignorando outras pastas, inclusive '...' e '..' */
      if (cur_ent->d_type == DT_DIR) continue;

      /* Contando mais uma mensagem existente */
      current_mailbox.exists++;

      /* Separando flags do nome do arquivo */
      char* info = strtok(cur_ent->d_name, ":");
      info = strtok(NULL, ":\r\n");
      if (strlen(info) <= 2)
        if (current_mailbox.unseen < 0) current_mailbox.unseen = current_mailbox.exists - 1; /* Marcando primeira mensagem não vista */
      if (strlen(info) > 2){
        if (info[2] == 'S') current_mailbox.flags[SEEN] = 1;
        else if (current_mailbox.unseen < 0) current_mailbox.unseen = current_mailbox.exists - 1; /* Marcando primeira mensagem não vista */
        if (info[2] == 'T') current_mailbox.flags[DELETED] = 1;
      } 
      if (strlen(info) > 3) 
        if (info[3] == 'T') current_mailbox.flags[DELETED] = 1;
    }
    closedir(mailbox_cur_directory); /* Fechando ponteiro de pasta aberto */
  } else{
    /* aconteceu um erro, retornando uma struct com sinal de erro */
    current_mailbox.status = -1;
    return current_mailbox;
  }

  return current_mailbox;
}

int main (int argc, char **argv) {
   /* Os sockets. Um que será o socket que vai escutar pelas conexões
    * e o outro que vai ser o socket específico de cada conexão */
	int listenfd, connfd;
   /* Informações sobre o socket (endereço e porta) ficam nesta struct */
	struct sockaddr_in servaddr;
   /* Retorno da função fork para saber quem é o processo filho e quem
    * é o processo pai */
   pid_t childpid;
   /* Armazena linhas recebidas do cliente */
	char	recvline[MAXLINE + 1];
   /* Armazena o tamanho da string lida do cliente */
   ssize_t  n;
   
	if (argc != 2) {
      fprintf(stderr,"Uso: %s <Porta>\n",argv[0]);
      fprintf(stderr,"Vai rodar um servidor de echo na porta <Porta> TCP\n");
		exit(1);
	}

   /* Criação de um socket. Eh como se fosse um descritor de arquivo. Eh
    * possivel fazer operacoes como read, write e close. Neste
    * caso o socket criado eh um socket IPv4 (por causa do AF_INET),
    * que vai usar TCP (por causa do SOCK_STREAM), já que o IMAP
    * funciona sobre TCP, e será usado para uma aplicação convencional sobre
    * a Internet (por causa do número 0) */
	if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		perror("socket :(\n");
		exit(2);
	}

   /* Agora é necessário informar os endereços associados a este
    * socket. É necessário informar o endereço / interface e a porta,
    * pois mais adiante o socket ficará esperando conexões nesta porta
    * e neste(s) endereços. Para isso é necessário preencher a struct
    * servaddr. É necessário colocar lá o tipo de socket (No nosso
    * caso AF_INET porque é IPv4), em qual endereço / interface serão
    * esperadas conexões (Neste caso em qualquer uma -- INADDR_ANY) e
    * qual a porta. Neste caso será a porta que foi passada como
    * argumento no shell (atoi(argv[1]))
    */
	bzero(&servaddr, sizeof(servaddr));
	servaddr.sin_family      = AF_INET;
	servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
	servaddr.sin_port        = htons(atoi(argv[1]));
	if (bind(listenfd, (struct sockaddr *)&servaddr, sizeof(servaddr)) == -1) {
		perror("bind :(\n");
		exit(3);
	}

   /* Como este código é o código de um servidor, o socket será um
    * socket passivo. Para isto é necessário chamar a função listen
    * que define que este é um socket de servidor que ficará esperando
    * por conexões nos endereços definidos na função bind. */
	if (listen(listenfd, LISTENQ) == -1) {
		perror("listen :(\n");
		exit(4);
	}

   printf("[Servidor no ar. Aguardando conexoes na porta %s]\n",argv[1]);
   printf("[Para finalizar, pressione CTRL+c ou rode um kill ou killall]\n");
   
   /* O servidor no final das contas é um loop infinito de espera por
    * conexões e processamento de cada uma individualmente */
	for (;;) {
      /* O socket inicial que foi criado é o socket que vai aguardar
       * pela conexão na porta especificada. Mas pode ser que existam
       * diversos clientes conectando no servidor. Por isso deve-se
       * utilizar a função accept. Esta função vai retirar uma conexão
       * da fila de conexões que foram aceitas no socket listenfd e
       * vai criar um socket específico para esta conexão. O descritor
       * deste novo socket é o retorno da função accept. */
		if ((connfd = accept(listenfd, (struct sockaddr *) NULL, NULL)) == -1 ) {
			perror("accept :(\n");
			exit(5);
		}
      
      /* Agora o servidor precisa tratar este cliente de forma
       * separada. Para isto é criado um processo filho usando a
       * função fork. O processo vai ser uma cópia deste. Depois da
       * função fork, os dois processos (pai e filho) estarão no mesmo
       * ponto do código, mas cada um terá um PID diferente. Assim é
       * possível diferenciar o que cada processo terá que fazer. O
       * filho tem que processar a requisição do cliente. O pai tem
       * que voltar no loop para continuar aceitando novas conexões */
      /* Se o retorno da função fork for zero, é porque está no
       * processo filho. */
      if ( (childpid = fork()) == 0) {
         /**** PROCESSO FILHO ****/
         printf("[Uma conexao aberta]\n");
         /* Já que está no processo filho, não precisa mais do socket
          * listenfd. Só o processo pai precisa deste socket. */
         close(listenfd);
         
         /* Agora pode ler do socket e escrever no socket. Isto tem
          * que ser feito em sincronia com o cliente. Não faz sentido
          * ler sem ter o que ler. Ou seja, neste caso está sendo
          * considerado que o cliente vai enviar algo para o servidor.
          * O servidor vai processar o que tiver sido enviado e vai
          * enviar uma resposta para o cliente (Que precisará estar
          * esperando por esta resposta) 
          */

         /* ========================================================= */
         /* ========================================================= */
         /*                         EP1 INÍCIO                        */
         /* ========================================================= */
         /* ========================================================= */
         /* EP1: todo cliente entra como NOT_AUTHENTICATED (não existe
          * pre-auth) */
         char client_state = NOT_AUTHENTICATED;
         char logged_user[MAXSTRING]; /* Usuario atualmente autenticado */

         mailbox current_mailbox; /* Mailbox atualmente aberta */
         strcpy(current_mailbox.path, "#NONE");

         while ((n=read(connfd, recvline, MAXLINE)) > 0) {
            /* Tratando comando vazio */
            if (n <= 2){
                /* Respondendo ao cliente */
                char response[MAXSTRING];
                sprintf(response, "* BAD empty command\n");
                write(connfd, response, strlen(response));
                continue;              
            }

            recvline[n]=0;
            printf("[Cliente %d enviou:] ",getpid());
            if ((fputs(recvline,stdout)) == EOF) {
               perror("fputs :( \n");
               exit(6);
            }

             /* Determinando qual o comando enviado */
            char* tag = strtok(recvline, " \r\n");
            char* command = strtok(NULL, " \r\n");

            /* Tratando comandos mal formados */
            if (tag == NULL || command == NULL){
              /* Respondendo ao cliente */
              char response[MAXSTRING];
              sprintf(response, "* BAD malformed command\n");
              write(connfd, response, strlen(response));
            }

            for (char *p = command ; *p; ++p) *p = toupper(*p);

            if (strcmp(command, "LOGIN") == 0){
              /* Realizar LOGIN do usuario */
              char* username = strtok(NULL, " \r\n");
              char* password = strtok(NULL, " \r\n");

              /* Verificando se o comando é válido */
              if (username == NULL || password == NULL){ /* comando mal formado */
                /* Respondendo ao cliente */
                char response[MAXSTRING];
                sprintf(response, "%s BAD LOGIN arguments invalid\n", tag);
                write(connfd, response, strlen(response));
              } else {
                /* Verificando se o usuário é válido */
                char result = attempt_login(username, password);
                if (result == 1){ /* Usuário válido */
                  /* Autenticando o usuario no processo */
                  client_state = AUTHENTICATED;
                  strcpy(logged_user, username);
                  printf("[Usuario \'%s\' logado no cliente %d]\n", logged_user, getpid());

                  /* Respondendo ao cliente */
                  char response[MAXSTRING];
                  sprintf(response, "%s OK LOGIN completed\n", tag);
                  write(connfd, response, strlen(response));
                  
                } else { /* Usuário inválido */
                  /* Respondendo ao cliente */
                  char response[MAXSTRING];
                  sprintf(response, "%s NO LOGIN completed\n", tag);
                  write(connfd, response, strlen(response));
                }
              }
            }
            else if (strcmp(command, "LOGOUT") == 0){
              /* Realizar LOGOUT do usuario, desconectar */
              /* Respondendo ao cliente */
              char response[MAXSTRING];
              sprintf(response, "* BYE rlServer logging out\n");
              write(connfd, response, strlen(response));
              sprintf(response, "%s OK LOGOUT completed\n", tag);
              write(connfd, response, strlen(response));
              close(connfd);
            }
            else if (strcmp(command, "SELECT") == 0){
              /* Selecionar uma das mailboxes disponíveis */
              char* mailbox_name = strtok(NULL, " \r\n");

              /* Verificando se o comando é válido */
              if (mailbox_name == NULL){ /* comando mal formado */
                /* Respondendo ao cliente */
                char response[MAXSTRING];
                sprintf(response, "%s BAD SELECT argument invalid\n", tag);
                write(connfd, response, strlen(response));
              } else if (client_state == NOT_AUTHENTICATED){ /* cliente não logado */
                /* Respondendo ao cliente */
                char response[MAXSTRING];
                sprintf(response, "%s BAD SELECT unauthenticated user\n", tag);
                write(connfd, response, strlen(response));
              } else {

                /* Abrir a mailbox e obter as informações */
                current_mailbox = open_mailbox(mailbox_name, logged_user);

                /* Verificação de erro na abertura da mailbox */
                if (current_mailbox.status < 0){
                  /* Respondendo ao cliente */
                  printf("[Erro selecionando a mailbox %s do usuario %s no cliente %d]\n", mailbox_name, logged_user, getpid());
                  char response[MAXSTRING];
                  sprintf(response, "%s BAD SELECT mailbox directory error\n", tag);
                  write(connfd, response, strlen(response));
                }

                /* Abrindo a mailbox no processo */
                client_state = SELECTED;
                printf("[Usuario \'%s\' selecionou a mailbox \'%s\' no cliente %d]\n", logged_user, mailbox_name, getpid());

                /* Respondendo ao cliente */
                char response[MAXSTRING];
                sprintf(response, "* %d EXISTS\n", current_mailbox.exists);
                write(connfd, response, strlen(response));
                sprintf(response, "* %d RECENT\n", current_mailbox.recent);
                write(connfd, response, strlen(response));
                sprintf(response, "* OK [UNSEEN %d] message %d is first unseen\n", current_mailbox.unseen, current_mailbox.unseen);
                write(connfd, response, strlen(response));
                if (current_mailbox.flags[0] == 1 || current_mailbox.flags[1] == 1){
                  char* flags_string = build_flags_string(current_mailbox.flags);
                  sprintf(response, "* FLAGS (%s)\n", flags_string);
                  write(connfd, response, strlen(response));
                  free(flags_string);
                }
                sprintf(response, "%s OK [READ-WRITE] SELECT completed\n", tag);
                write(connfd, response, strlen(response));
                
              }
            }

            else { /* Comando não reconhecido *q
              /* Respondendo ao cliente */
              char response[MAXSTRING];
              sprintf(response, "%s BAD %s command unknown or arguments invalid\n", tag, command);
              write(connfd, response, strlen(response));
            }
            //write(connfd, recvline, strlen(recvline));
         }
         /* ========================================================= */
         /* ========================================================= */
         /*                         EP1 FIM                           */
         /* ========================================================= */
         /* ========================================================= */

         /* Após ter feito toda a troca de informação com o cliente,
          * pode finalizar o processo filho */
         printf("[Uma conexao fechada]\n");
         exit(0);
      }
      /**** PROCESSO PAI ****/
      /* Se for o pai, a única coisa a ser feita é fechar o socket
       * connfd (ele é o socket do cliente específico que será tratado
       * pelo processo filho) */
		close(connfd);
	}
	exit(0);
}
