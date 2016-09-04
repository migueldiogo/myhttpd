//
//  main.c
//  SO_Projeto2014
//
//  Created by Miguel Prata Leal Branco Diogo - No.Uc: 2013130893 on 07/11/14.
//  Copyright (c) 2014 Miguel Diogo. All rights reserved.
//

#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/shm.h>
#include <semaphore.h>
#include <pthread.h>
#include <time.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <errno.h>	

#define DEBUG           1                               // retirar para ocultar informacao de debug na consola

#define SERVER_PATH     "/mnt/hgfs/SO/Projeto/SO_Projeto2014/SO_Projeto2014/"
#define FILE_PATH_BUF  254

/* Modelos para HTTP Replies feitos pelo servidor */
#define HEADER_1        "HTTP/1.0 "                     // Primeira linha da resposta do servidor...
#define MESSAGE_200     "200 OK\r\n"                    // ... + Resposta se o pedido do cliente for aceite
#define MESSAGE_204     "204 No Content\r\n"                    // ... + Resposta se o pedido do cliente for aceite
#define MESSAGE_400     "400 Bad Request\r\n"
#define MESSAGE_401     "401 Unauthorized\r\n"
#define MESSAGE_404     "404 Not Found\r\n"
#define MESSAGE_500     "500 Internal Server Error\r\n"     // ... + Resposta se o pedido do cliente for recusado
#define MESSAGE_503     "503 Service Unavailable\r\n"     // ... + Resposta se o pedido do cliente for recusado devido a lotacao do seridor


#define SERVER_STRING   "Server: httpd/0.1.0\r\n"       // Nome do servidor
#define CONTENT_TYPE    "Content-Type: "                // Tipo do conteudo/resposta

/* Modelos para HTTP Requests feitos pelo cliente */
#define GET_EXPR        "GET"                           // Expressão GET vinda dos clientes
#define CGI_EXPR        "cgi-bin/"                      // extensao do diretorio para acesso a conteudo dinamico
#define SIZE_BUF        1024                            // tamanho do buffer para pedidos e para respostas
#define SCRIPTS_MAX     10                              // numero de scripts maximo permitido

#define LIMITE_WAIT_TIME    4



/**** VARIAVEIS GLOBAIS ***********************************************************************/


/* Estrutura partilhada populada pelas configuracoes do servidor */
struct c {
    sem_t sem_config;
    int serv_port;                                      // porta do servidor usada para escutar
    int thread_pool_dim;                                // numero maximo de threads a serem usadas
    char sched_policy;                                   // politica de priorizacao do tipo de pedidos vindos do cliente
    char allowed_scripts[SCRIPTS_MAX][64];              // vetor populado com os scripts permitidos a pedido do cliente
    int allowed_scripts_num;                            // numero de scripts permitidos localizados no ficheiro de configuracao do servidor
};

typedef struct c config;

/* memoria partilhada - configuracoes */
int shmem_id_config;                                    // id da memoria partilhada da estrutura de configuracoes do servidor
config* config_info = NULL;                              // ponteiro para estrutura de dados com as configuracoes do sistema



/* posix pthread variables */
pthread_mutex_t mutex_config_shmem = PTHREAD_MUTEX_INITIALIZER;     // mutex para as configuracoes do servidor em memoria partilhada
pthread_t thread_receiver;
pthread_t thread_scheduler;
pthread_t* thread_pool;                                 // ponteiro para espaco em memoria que aloca o conjunto de threads na pool
int* thread_pool_id;                                    // ponteiro para espaco em memoria que aloca o conjunto de id's das threads na pool
int thread_pool_dim = 0;

sem_t sem_free_threads;

/* variaveis partilhadas pelas threads 'Receiver' e 'Scheduler' */
char request_string[SIZE_BUF];                          // aqui e' armazenada de cada vez cada pedido em forma bruta vindo do exterior

/* Message Queue*/
typedef struct {
    int mtype;                                          // tipo da mensagem a ser comunicada
    int thread_id;                                      // id da thread que envia a mensagem
    char tipo_pedido;                                   // tipo do pedido (estatico ou dinamico)
    char ficheiro[256];                                 // ficheiro associado ao pedido
    time_t tempo_ini;                                   // hora de inicio do processamento do pedido
    time_t tempo_fim;                                   // hora de fim do processamento do pedido
} thread_msg;

/* estrutura do servidor */
typedef struct {
    int servidor_on;                                    // estado do servidor ( desligar(-1), reiniciar(0), ligado(1) )
    sem_t msgqueue_ready;                               // semaforo para assegurar que o servico de estatisticas e sua queue esta pronta
    time_t time_bootup;                                 // hora de arranque do servidor
    time_t time_shutdown;                               // hora de encerramento do servidor
    int total_estaticos;                                // total pedidos estaticos
    int total_dinamicos;                                // total pedidos dinamicos
    int total_recusados;                                // total pedidos perdidos
    
} servidor;


int shmem_id_servidor;                                    // id da memoria partilhada da estrutura de configuracoes do servidor
servidor* servidor_info = NULL;                              // ponteiro para estrutura de dados com as configuracoes do sistema

pthread_mutex_t mutex_servidor_shmem = PTHREAD_MUTEX_INITIALIZER;


int msgid = 0;





/* REQUESTS*/

/* Estrutura molde para uma entrada no 'Request Buffer' */
typedef struct r {
    int socket;                                         // socket do cliente
    char ficheiro[FILE_PATH_BUF];                                 // ficheiro pedido pelo cliente
    char tipo_conteudo;                                  // tipo de conteudo: 'e' -> estatico; 'd' -> CGI; 'z' -> Invalido
    int codigo_resposta;                                // resposta associada ao pedido (mensagens de erro ou sucesso devolvidas a cliente)
} Request;

Request** requests_buffer;
int requests_buffer_dim = 0;

Request* request_alvo = NULL;                                              // variavel onde as threads recorrem para levantarem o pedido que lhes e' destinado
pthread_cond_t cond_requestalvo = PTHREAD_COND_INITIALIZER;
pthread_mutex_t mutex_requestalvo = PTHREAD_MUTEX_INITIALIZER;

sem_t sem_requestsbuff_empty;
sem_t sem_requestsbuff_full;

pthread_mutex_t mutex_requestsbuffer = PTHREAD_MUTEX_INITIALIZER;


/* operacoes lista de requests*/


int testa_servidor_estado();

void inicia_requestList() {
    int i;
    requests_buffer = (Request**) calloc(requests_buffer_dim, sizeof(Request));
    sem_init(&sem_requestsbuff_empty, 1, requests_buffer_dim);
    sem_init(&sem_requestsbuff_full, 1, 0);

    
    for (i = 0; i < requests_buffer_dim; i++) {
        requests_buffer[i] = NULL;
    }
}

int insere_request(Request* pedido) {
    int i;
    int encontrado = 0;
    int sucesso = 0;

    if (sem_trywait(&sem_requestsbuff_empty) != -1) {
        sucesso = 1;
        pthread_mutex_lock(&mutex_requestsbuffer);
        for(i = 0; i < requests_buffer_dim && encontrado == 0; i++) {
            if (requests_buffer[i] == NULL) {
                encontrado = 1;
                requests_buffer[i] = pedido;
                sem_post(&sem_requestsbuff_full);

            }

        }
        printf("=====\n");
        for(i = 0; i<requests_buffer_dim; i++) {
            printf("%p\t", requests_buffer[i]);
        }
        printf("\n======\n");


        pthread_mutex_unlock(&mutex_requestsbuffer);
    }
    else
        sucesso = 0;
    
    return sucesso;

      
}

int elimina_request(Request* pedido) {
    int i;
    int encontrado = 0;
    

    for (i = 0; i < requests_buffer_dim && encontrado == 0; i++) {
        if (requests_buffer[i]->socket == pedido->socket &&
                requests_buffer[i]->tipo_conteudo == pedido->tipo_conteudo &&
                strcmp(requests_buffer[i]->ficheiro, pedido->ficheiro)==0) {
            encontrado = 1;
        }
    }
    
    if (encontrado == 1) {
        /* shift para a esquerda de elementos 'a direita do elemento eliminado*/
        while (i < requests_buffer_dim && requests_buffer[i] != NULL) {
            requests_buffer[i-1] = requests_buffer[i];
            i++;
        }
        if (requests_buffer[i] == NULL)
            requests_buffer[i-1] = NULL;
        
        sem_post(&sem_requestsbuff_empty);

    }

       
    return encontrado;
    
    
}

Request* procura_pedido(char tipo_conteudo) {
    int i;
    int encontrado = 0;
    Request* pedido = NULL;
    
    while(testa_servidor_estado() == 1 && sem_trywait(&sem_requestsbuff_full) == -1){};
    
    
    if (testa_servidor_estado() == 1) {
        pthread_mutex_lock(&mutex_requestsbuffer);

        if (tipo_conteudo = 't') {
            pedido = requests_buffer[0];
        }
        else {
            for (i = 0; i < requests_buffer_dim && encontrado == 0; i++) {
                if (requests_buffer[i]->tipo_conteudo == tipo_conteudo) {
                    encontrado = 1;
                    pedido = requests_buffer[i];
                }
            }
        }
        elimina_request(pedido);
        pthread_mutex_unlock(&mutex_requestsbuffer);
    }
    return pedido;
    
}



/* FIM DE VARIAVEIS GLOBAIS *********************************************************************/



/* FEITO */
/* funcao que imprime erros fatais do programa */
void erro_fatal(char* mensagem) {
    perror(mensagem);
    exit(EXIT_FAILURE);
}

void cleanup() {
    // desaloca memorias partilhadas
    shmctl(shmem_id_config, IPC_RMID, NULL);
    shmctl(shmem_id_servidor, IPC_RMID, NULL);
    
    // desaloca espacos de memoria dinamicos
    free(thread_pool);
    free(thread_pool_id);
    free(requests_buffer);

    // destroi semaforos
    sem_destroy(&sem_free_threads);
    sem_destroy(&sem_requestsbuff_empty);
    sem_destroy(&sem_requestsbuff_full);

}







/* FEITO */
/* cria ou atualiza a estrutura armazenada em memoria partilhada que traduz o ficheiro de configuracao do servidor */
void configuration_manager() {
    char scripts_row[1024];                             // recebe todos os scripts permitidos, vindos do ficheiro diretamente
    char* token;                                        // percorre todos os scripts em "scripts_row"
    int i;
    char config_file_path[150] = "";
    FILE* fp;
    
    sleep(2);
    strcat(strcat(config_file_path, SERVER_PATH), "config.txt");
    
    
    fp = fopen(config_file_path, "r");                // abre para leitura o ficheiro de configuracoes do servidor
    
    if (fp == NULL)
        erro_fatal("ERRO a abrir o ficheiro de configuracoes do servidor");
    
    #ifdef DEBUG
    printf("[Gestor de Configuracoes] A carregar ficheiro de configuracoes do servidor para memoria...\n");
    #endif
    
    config_info = (config*) shmat(shmem_id_config, NULL, 0);
    
    pthread_mutex_lock(&mutex_config_shmem);            // tenta aceder 'a memoria partilhada, consegue se ninguem estiver a usa-la
    /* Inicio da zona critica */
    
    /* Leitura e interpretacao do ficheiro de configuracao do servidor, seguinda de populacao da estrutura partilhada */
    fscanf(fp, " serv_port=%d", &config_info->serv_port);
    fscanf(fp, " thread_pool_dim=%d", &config_info->thread_pool_dim);
    fscanf(fp, " sched_policy=%c", &config_info->sched_policy);
    fscanf(fp, " allowed_scripts=%s", scripts_row);
   
    config_info->allowed_scripts_num = 0;               // iniciando a contagem de scripts presentes em config file
    token = strtok(scripts_row, ";");                   // armazena todos os scipts permitidos, separados por ";"
    
    /* ciclo que separa os scripts um dos outros e os indidualiza num array presente na estrutura em memoria partilhada */
    for (i = 0; i < SCRIPTS_MAX && token != NULL ; i++) {
        strcpy(config_info->allowed_scripts[i], token); // armazena um novo script
        token = strtok(NULL, ";");                      // salta para o proximo token
        (config_info->allowed_scripts_num)++;           // incrementa a contagem dos scripts presentes no ficheiro, guarda na estrutura partilhada
    }
    
    /* Fim da zona critica */
    pthread_mutex_unlock(&mutex_config_shmem);          // disponibiliza o acesso 'a memoria partilhada
    
    fclose(fp);
    
    #ifdef DEBUG
    printf("[Gestor de Configuracoes] Ficheiro de configuracoes do servidor carregado\n");
    #endif

    sem_post(&config_info->sem_config);

}

/* FEITO */
void identifica(int socket_online, int* port_cliente, char* ipstr_cliente) {
    socklen_t socket_len;                               // dimensao do socket
    
    struct sockaddr_in * socket_addrin;                 // criacao de estrutura addr_in que vai interpretar a estruta addr_storage abaixo
    struct sockaddr_storage addr_storage;;              // criacao de estrutura addr_storage que contem o dominio do socket e a porta do cliente
    socket_len = sizeof(struct sockaddr_storage);
    getpeername(socket_online, (struct sockaddr*) &addr_storage, &socket_len);              // interpretacao do socket_online que resulta em info da maquina do cliente
    
    /* assunmindo IPv4 */
    socket_addrin = (struct sockaddr_in *) &addr_storage;                                   // conversao do addr_storage em addr_in
    *port_cliente = ntohs(socket_addrin->sin_port);                                         // extracao da porta da estrutura para uma variavel
    inet_ntop(AF_INET, &socket_addrin->sin_addr, ipstr_cliente, INET_ADDRSTRLEN);     // extracao da string do ip da estrutura para uma variavel
}

/* FEITO */
int readline(int socket, int porta, char* ipstr) {
    int i;
    char char_lido = '\0';                              // deposito de cada char da mensagem
    int end_of_file = 0;                                // 1 se EOF
    int nread = 0;                                // numero de caracteres lidos no total antes de mudanca de linha '\r'
    int ret;
    
   
    
    while (nread<SIZE_BUF && end_of_file == 0) {
        ret = read(socket, &char_lido, sizeof(char));

        if (ret == -1) {
            return -1;
        }
        else if (ret == 0) {
            return 0;
        }
        else if (char_lido == '\r') {
            end_of_file = 1;
        }
        else {
            request_string[nread] = char_lido;
            nread++;
        }
    }
    
    request_string[nread] = '\0';
    
    
    
    return nread;
}

/* FEITO */
int request_handler(int socket, int porta, char* ipstr) {
    char comando[16];
    char full_filename[512];
    char filename[256];
    char formato[16];
    char versao_http[16];
    char* token;
    int content_analyser = 0;                                                           // 0: se estatico; 1 se CGI; -1 se nao suportado/acesso negado
    int diretorio_cgi = 0;
    
    
    int requests_buffer_size = 0;
    
    char reply_buffer[SIZE_BUF];
    
    
    Request* pedido = (Request*) malloc(sizeof(Request));                                //molde a ser preenchido pelo pedido e a ser entregue ao 'Request Buffer'
    
    pedido->socket = socket;                                                             // guarda o socket da ligacao

    #ifdef DEBUG
    printf("[RECEIVER] Pedido recebido: %s\n", request_string);
    #endif
    
    sscanf(request_string, "%s %s %s", comando, full_filename, versao_http);            // extrai e analisa a string de pedido vinda do client
    
    if(strstr(full_filename, "/cgi-bin/") != NULL) {
        sscanf(full_filename, "/cgi-bin/%s", filename);            // formato do pedido para CGI
        diretorio_cgi = 1;
    }    
    else
        sscanf(full_filename, "/%s", filename);                 // formato do pedido para conteudo estatico

    if (strstr(filename, "/") == NULL) {                        // proteger o programa contra acessos a outros diretorios
        /* extracao da extensao do ficheiro */
        token = strtok(filename, ".");
        token = strtok(NULL, ".");

        /* depois de ser extraida a extensao, recupera-se o nome original do ficheiro (nome.extensao) */
        strcpy(formato, token);
        strcat(filename, ".");
        strcat(filename, formato);

        /* analise do ficheiro pedido */
        if (strcmp(formato, "html") == 0) {
            content_analyser = 0;                               // valor correspondente ao tipo de conteudo (0 para .html)
            strcpy(pedido->ficheiro, filename);                 // preenche o campo com o nome do ficheiro (ex: index.html)
            pedido->tipo_conteudo = 'e';                          // preenche o campo com o tipo de conteudo 'e' estatico
            pedido->codigo_resposta = 200;
            
            #ifdef DEBUG
            printf("[RECEIVER] Pedido %s detetado.\n", formato);
            #endif
        }
        
        else if (strcmp(formato, "sh") == 0) {
            content_analyser = 1;                       // valor correspondente ao tipo de conteudo (1 para .sh)
            sprintf(pedido->ficheiro, "%s", filename);          //  nome do ficheiro (ex: script1.sh)
            if (diretorio_cgi == 1)
                pedido->tipo_conteudo = 'd';                   // preenche o campo com o tipo de conteudo 'd' dinamico
            else
                pedido->tipo_conteudo = 'z';                   // preenche o campo com o tipo de conteudo 'd' dinamico
 
            
            pedido->codigo_resposta = 200;
            #ifdef DEBUG
            printf("[RECEIVER] Pedido %s detetado.\n", formato);
            #endif
        }
            
        
        else {
            strcpy(pedido->ficheiro, filename);         
            content_analyser = -1;                              // se nao é .html nem .sh -> valor de erro
            pedido->codigo_resposta = 500;
        }
    }
    
    else {
        content_analyser = -1;                                  // se o cliente quer aceder a outros diretorios -> valor de erro
        pedido->codigo_resposta = 500;

    }
        

    if (insere_request(pedido) == 0) {
        pedido->codigo_resposta = 503;
        
        /* Header */
        sprintf(reply_buffer, HEADER_1);
        send(socket, reply_buffer, strlen(reply_buffer), 0);
        /* Informacao sobre o servidor e conteudo */
        sprintf(reply_buffer, SERVER_STRING);
        send(socket, reply_buffer, strlen(reply_buffer), 0);
        
        sprintf(reply_buffer, MESSAGE_503);
        send(socket, reply_buffer, strlen(reply_buffer), 0);

        /* mensagem de erro, se servidor sobrecarregado */
        sprintf(reply_buffer, "\r\n");
        send(socket, reply_buffer, strlen(reply_buffer), 0);
        sprintf(reply_buffer, "<HTML><TITLE>SERVIDOR SOBRECARREGADO</TITLE>\r\n");
        send(socket, reply_buffer, strlen(reply_buffer), 0);
        sprintf(reply_buffer, "<BODY><P>Servidor temporariamente sobrecarregado. Tente novamente mais tarde.\r\n");
        send(socket, reply_buffer, strlen(reply_buffer), 0);
        sprintf(reply_buffer, "</BODY></HTML>\r\n");
        send(socket, reply_buffer, strlen(reply_buffer), 0);
     

        /* termina comunicacao com cliente de forma ordeira */
        shutdown(socket, SHUT_WR);
        recv(socket, reply_buffer, sizeof(reply_buffer), 0);
        sleep(2);
        close(socket);
        
        pthread_mutex_lock(&mutex_config_shmem);
        (servidor_info->total_recusados)++;
        pthread_mutex_unlock(&mutex_servidor_shmem);
        
    }

    return content_analyser;
    
}


/* FEITO */
void* receiver_main(void* socket_ini) {
    int socket_inicial = *((int* )socket_ini);
    int socket_online;                                  // novo socket id com ligacao estabelecida
    struct sockaddr_in client_addr;                     // estrutura que define o no de ligacao a ser preenchida pelo cliente recebido
    socklen_t client_addr_len = sizeof(client_addr);    // dimensao da estrutura que define o endereco do cliente
    
    int port_cliente;                                   // porta do cliente
    char ipstr_cliente[INET_ADDRSTRLEN];               // cria array para alojar a string do ip do cliente
    
    inicia_requestList();             // cria array com requests
    
    while (1) {

        #ifdef DEBUG
        printf("[RECEIVER] Pronto a aceitar nova conexao...\n");
        #endif
        /* espera por ligacao a novo cliente */
        if ((socket_online = accept(socket_inicial, (struct sockaddr *) &client_addr, &client_addr_len)) == -1) {
            sleep(4);
            close(socket_inicial);
            close(socket_online);
            if(testa_servidor_estado() < 1) 
                return NULL;
            else
                erro_fatal("Erro a aceitar nova coneccao de novo cliente");
        }

        identifica(socket_online, &port_cliente, ipstr_cliente);    // identifica o IP e a porta do novo cliente
        
        #ifdef DEBUG
        printf("[RECEIVER] Nova conexao aceite vinda de %s:%d Socket: %d\n", ipstr_cliente, port_cliente, socket_online);
        #endif
        
        readline(socket_online, port_cliente, ipstr_cliente);                        // le primeira linha do pedido do cliente
        request_handler(socket_online, port_cliente, ipstr_cliente);                 // analisa o pedido do cliente e formata-o
       
    }
    #ifdef DEBUG
    printf("[RECEIVER] A terminar Receiver...\n");
    #endif

    close(socket_online);
    
    
    return NULL;
}



void* scheduler_main() {
    int i;
                                                                // molde a ser preenchido pelo pedido e a ser entregue ao 'Request Buffer'
    char file_path[FILE_PATH_BUF] = "";
    int valido = 0;                                             // ACHO QUE NAO E' MUITO UTIL- RETIRAR NO FUTURO
    FILE* fp;
    char policy = '\0';
    
    Request* pedido;

    char file_cgi[SIZE_BUF];
    
    #ifdef DEBUG
    printf("[SCHEDULER] A iniciar Scheduler...\n");
    #endif

    /* inicializacao de semaforos usados na tread receiver e scheduler */    

    sleep(3);
    
    while (testa_servidor_estado() == 1) {                      // trabalha enquanto servidor ligado
        sprintf(file_path, "%s", SERVER_PATH);
        pthread_mutex_lock(&mutex_config_shmem);
        policy = config_info->sched_policy;                     // armazena a politica de escalonamento localmente (mais seguro, sem possiveis race conditions) 
        pthread_mutex_unlock(&mutex_config_shmem);

        
        /* CONSUMIDOR DE REQUESTS */
        pedido = procura_pedido(policy);                        // procura o proximo pedido de tipo t ('e' para estatico, 'd' par CGI)
        
        if (pedido == NULL) {                                   // se nao encontra nenhum pedido prioritario, invoca a politica de FIFO
            pedido = procura_pedido('t');                       // procura qualquer pedido, visto que o tipo 't' ninguem tem, vai buscar o primeiro pedido no buffer
        }
        
        if(testa_servidor_estado() < 1) {                       // certifica-se que o servidor nao esta a desligar antes de continuar
            continue;
        }
        
        if (pedido != NULL) {                                   // se encontrei um pedido para trabalhar
            pthread_mutex_lock(&mutex_requestalvo);
            request_alvo = pedido;                              // ponho-o a vista das pool das threads, para poderem trabalhar nele
            pthread_mutex_unlock(&mutex_requestalvo);

        }

        strcat(file_path, request_alvo->ficheiro);

        


        if (request_alvo->tipo_conteudo == 'e') {


            if ((fp = fopen(file_path, "r")) != NULL) {
                valido = 1;

                /* incrementa estatistica dos pedidos estaticos*/
                pthread_mutex_lock(&mutex_servidor_shmem);
                (servidor_info->total_estaticos)++;
                pthread_mutex_unlock(&mutex_servidor_shmem);

                #ifdef DEBUG
                    printf("[Scheduler] Novo pedido: %s - Pedido Válido.\n", file_path);
                #endif
                fclose(fp);
            }
            else {
                valido = 0;
                #ifdef DEBUG
                    printf("[Scheduler] Novo pedido: %s - Ficheiro Não Encontrado.\n", file_path);
                #endif
                request_alvo->codigo_resposta = 404;
            }
        }
        else if (request_alvo->tipo_conteudo == 'd') {
            valido = 0;
            pthread_mutex_lock(&mutex_config_shmem);
            /* ciclo percorre o array de scripts permitidos na estrutura partilhada */
            for (i = 0; i < config_info->allowed_scripts_num && valido == 0; i++) {
                /* se o script pedido se encontra no array de script permitidos carregado do ficheiro config e localizado em memoria partilhada */
                if (strcmp(request_alvo->ficheiro, config_info->allowed_scripts[i]) == 0) {
                    valido = 1;  

                    /* incrementa estatistica dos pedidos dinamicos*/
                    pthread_mutex_lock(&mutex_servidor_shmem);
                    (servidor_info->total_dinamicos)++;
                    pthread_mutex_unlock(&mutex_servidor_shmem);

                    // preenche o campo com o tipo de conteudo (ver legenda/comentario para a var 'content_analyser')
                    //sprintf(file_cgi, "%s", request_alvo->ficheiro);
                    //sprintf(request_alvo->ficheiro, "%s%s", CGI_EXPR, file_cgi);
                    #ifdef DEBUG
                        printf("[Scheduler] Novo pedido: %s - Script Valido.\n", request_alvo->ficheiro);
                    #endif
                    request_alvo->codigo_resposta = 200;
                }
            }
            pthread_mutex_unlock(&mutex_config_shmem);

            if (valido == 0) {
                #ifdef DEBUG
                    printf("[Scheduler] Novo pedido: %s - Script Negado.\n", request_alvo->ficheiro);
                #endif
                    request_alvo->codigo_resposta = 401;

            }
        }
        else{
            valido = 0;
            #ifdef DEBUG
            printf("[Scheduler] Novo pedido: %s - Negado.\n", request_alvo->ficheiro);
            #endif
            request_alvo->codigo_resposta = 500;


        }

        sem_wait(&sem_free_threads);                        // espera por uma thread livre
        pthread_cond_signal(&cond_requestalvo);             // sinaliza uma thread que tem o resquest_alvo agora esta preenchido
        #ifdef DEBUG
        printf("[Scheduler] Pedido enviado 'a pool de Threads.\n");
        #endif
        pthread_mutex_unlock(&mutex_requestalvo);               

    }

    
    #ifdef DEBUG
    printf("[SCHEDULER] A terminar Scheduler...\n");
    #endif
    
    /* se scheduler esta a terminar e' porque o servidor esta a encerrar, precisamos de alertar todas as threads...*/
    /*... para verificarem a sua variavel condicional, para poderem acabar a sua rotina ordeiramente */
    pthread_mutex_lock(&mutex_requestalvo);               
    pthread_cond_broadcast(&cond_requestalvo);
    pthread_mutex_unlock(&mutex_requestalvo);               

    return NULL;
}


/* FEITO */
/* Envia pagina HTML para cliente */
void send_html_page(int threads_id, Request* pedido) {
    FILE* fp;
    char line_buffer[SIZE_BUF];
    int socket = pedido->socket;
    char file_path[FILE_PATH_BUF] = "";

    sprintf(file_path, "%s", SERVER_PATH);
    strcat(file_path, pedido->ficheiro);

    
    if ((fp = fopen(file_path, "r")) != NULL) {
        #ifdef DEBUG
        printf("[THREAD POOL %d] A abrir ficheiro HTML: %s...\n", threads_id, pedido->ficheiro);
        #endif       
    }
    #ifdef DEBUG
    printf("[THREAD POOL %d] A enviar ficheiro HTML de %s para cliente...\n", threads_id, pedido->ficheiro);
    #endif

    sprintf(line_buffer, "\r\n");
    send(socket, line_buffer, strlen(line_buffer), 0);
    while (fgets(line_buffer, SIZE_BUF, fp)) {
        send(socket, line_buffer, strlen(line_buffer), 0);
    }
    #ifdef DEBUG
    printf("[THREAD POOL %d] Ficheiro HTML de %s envio a cliente concluido\n", threads_id, pedido->ficheiro);
    #endif
    
    fclose(fp);
    
}

int testa_servidor_estado() {
    int resultado;
    
    pthread_mutex_lock(&mutex_servidor_shmem);
    
    resultado = servidor_info->servidor_on;
    pthread_mutex_unlock(&mutex_servidor_shmem);

    return resultado;
}





/* FALTA TRATAR CONTEUDO DINAMICO */
/* Funcao main de cada thread da pool para o acesso a I/O e envio de resposta a cliente */
void* pool_main(void* threads_id) {
    int t_id = *((int*)threads_id);
    char reply_buffer[SIZE_BUF];
    int socket;
    int client_port;
    char client_ipstr[INET_ADDRSTRLEN];
    char file_path[FILE_PATH_BUF];
    char sh_filename[FILE_PATH_BUF];
    int encontrado = 0;
    int i;
    
    Request* pedido;                                                            // copia de novo pedido para ser processado de forma segura
    
    time_t time_inicio;
    time_t time_fim;

    thread_msg threadmsg;
    
    int pipe_fd[2];                                                             // inicializacao de pipe para processamento de conteudo dinamico
    int pid;
    int nread = 1;

    
    while (testa_servidor_estado() == 1) {                                      // enquanto servidor esta ligado, a thread trabalha nos seus pedidos
        #ifdef DEBUG
        printf("[THREAD POOL %d] Á espera de receber trabalho...\n", t_id);
        #endif

        pthread_mutex_lock(&mutex_requestalvo);
        
        /* condicao de block/unblock */
        while (request_alvo == NULL && testa_servidor_estado() == 1) {          // se não ha pedidos novos e servidor continua ligado, thread adormece
            sem_post(&sem_free_threads);                                        // notifica scheduler que existe mais uma thread livre
            pthread_cond_wait(&cond_requestalvo, &mutex_requestalvo);           // variavel condicional para a existencia de novo pedido
        }
        if (request_alvo != NULL) {
            pedido = request_alvo;                      // copia pedido para variavel local, para request_alvo poder ser libertado
            request_alvo = NULL;                        // muda o valor para NULL para notificar que se encontra livre
        }
        pthread_mutex_unlock(&mutex_requestalvo);
        
        if (testa_servidor_estado() == 1) {             // testa estado do servidor mais uma vez antes de processar o pedido
            
            time(&time_inicio);                         // regista hora de inicio de processamento de pedido
            #ifdef DEBUG
            printf("[THREAD POOL %d] Acabei de receber trabalho para fazer!\n", t_id);
            #endif

            
            socket = pedido->socket;                    // copia do valor do socket

            /* identifica o endereco e porta do cliente */
            identifica(socket, &client_port, client_ipstr);


            #ifdef DEBUG
            printf("[THREAD POOL %d] A enviar resposta ao pedido a %s vindo de %s:%d...\n", t_id, pedido->ficheiro, client_ipstr, client_port);
            #endif


            /* Header */
            sprintf(reply_buffer, HEADER_1);
            send(socket, reply_buffer, strlen(reply_buffer), 0);

            /* Respostas consoante o sucesso da rececao do pedido */
            switch (pedido->codigo_resposta) {
                case 200:
                    sprintf(reply_buffer, MESSAGE_200);
                    send(socket, reply_buffer, strlen(reply_buffer), 0);
                    break;
                case 204:
                    sprintf(reply_buffer, MESSAGE_204);
                    send(socket, reply_buffer, strlen(reply_buffer), 0);
                    break;
                case 400:
                    sprintf(reply_buffer, MESSAGE_400);
                    send(socket, reply_buffer, strlen(reply_buffer), 0);
                    break;
                case 401:
                    sprintf(reply_buffer, MESSAGE_401);
                    send(socket, reply_buffer, strlen(reply_buffer), 0);
                    break;
                case 404:
                    sprintf(reply_buffer, MESSAGE_404);
                    send(socket, reply_buffer, strlen(reply_buffer), 0);
                    break;
                case 500:
                    sprintf(reply_buffer, MESSAGE_500);
                    send(socket, reply_buffer, strlen(reply_buffer), 0);
                    break;

                default:
                    sprintf(reply_buffer, MESSAGE_500);
                    send(socket, reply_buffer, strlen(reply_buffer), 0);
                    break;
            }

            /* Informacao sobre o servidor e conteudo */
            sprintf(reply_buffer, SERVER_STRING);
            send(socket, reply_buffer, strlen(reply_buffer), 0);






            /* Pagina HTML retornada ao cliente quando pedido invalido */
            if (pedido->codigo_resposta != 200 && pedido->codigo_resposta != 204) {
                sprintf(reply_buffer, "\r\n");
                send(socket, reply_buffer, strlen(reply_buffer), 0);
                sprintf(reply_buffer, "<HTML><TITLE>Not Found</TITLE>\r\n");
                send(socket, reply_buffer, strlen(reply_buffer), 0);
                sprintf(reply_buffer, "<BODY><P>Resource unavailable or nonexistent.\r\n");
                send(socket, reply_buffer, strlen(reply_buffer), 0);
                sprintf(reply_buffer, "</BODY></HTML>\r\n");
                send(socket, reply_buffer, strlen(reply_buffer), 0);

            }
            else if (pedido->tipo_conteudo == 'e' || pedido->tipo_conteudo == 'd'){
                /* processamento e envio de conteudo estatico */
                if (pedido->tipo_conteudo == 'e') {
                    if (pedido->codigo_resposta != 204) {
                        
                    /* envia tipo de conteudo (html) */
                    sprintf(reply_buffer, "%stext/html\r\n", CONTENT_TYPE);
                    send(socket, reply_buffer, strlen(reply_buffer), 0);
            }
                    send_html_page(t_id, pedido);                   // envia codigo de pagina html para cliente
                }

                /* processamento e envio de conteudo dinamico */
                else if (pedido->tipo_conteudo == 'd'){
                                        
                    sprintf(file_path, "%s%s", SERVER_PATH, CGI_EXPR);
                    strcat(file_path, pedido->ficheiro);
                    
                    if (pipe(pipe_fd) == -1) {
                        erro_fatal("Erro a iniciar pipe para leitura de CGI");
                    }
                    
                    pid = fork();
                    if (pid == -1)
                        erro_fatal("Erro a criar processo-filho para lidar com CGI");
                    
                    else if (pid == 0) {
                        while ((dup2(pipe_fd[1], STDOUT_FILENO) == -1) && (errno == EINTR)) {}
                        close(pipe_fd[1]);                      // fecha extremidade de escrita
                        close(pipe_fd[0]);                      // fecha extremidade de leitura
                        execl("/bin/sh", "sh", file_path, (char*)NULL);     // executa comando "sh ficheiro.sh"
                        perror("execl");
                        exit(1);  
                    }
                    
                    else {
                        close(pipe_fd[1]);                      // fecha extremidade de escrita

                        while(1) {
                            /* le conteudo vindo do processo-filho que executa o script */
                            nread = read(pipe_fd[0], reply_buffer, sizeof(reply_buffer) - 1);                           
                            reply_buffer[nread] = '\0';
                            
                            if (nread == -1)
                                if (errno == EINTR)
                                    continue;
                                else
                                    erro_fatal("ERRO a ler pipe");
                            else if (nread == 0)
                                break;
                            else {
                                send(socket, reply_buffer, strlen(reply_buffer), 0);
                            }


                        }
                        
                        close(pipe_fd[0]);                      // fecha extremidade de leitura
                        wait(NULL);
                        
                        
                        
                    }
                    
                }
       
            }
            /* se outra coisa - envia mensagem de erro a cliente */
            else {
                sprintf(reply_buffer, "\r\n");
                send(socket, reply_buffer, strlen(reply_buffer), 0);
                sprintf(reply_buffer, "<HTML><TITLE>Not Found</TITLE>\r\n");
                send(socket, reply_buffer, strlen(reply_buffer), 0);
                sprintf(reply_buffer, "<BODY><P>Resource unavailable or nonexistent.\r\n");
                send(socket, reply_buffer, strlen(reply_buffer), 0);
                sprintf(reply_buffer, "</BODY></HTML>\r\n");
                send(socket, reply_buffer, strlen(reply_buffer), 0);
            }
            
            /* encerra ordeiramente a comunicação entre a thread e o cliente (browser) */
            shutdown(socket, SHUT_WR);
            recv(socket, reply_buffer, sizeof(reply_buffer), 0);
            sleep(2);
            close(socket);

            
            time(&time_fim);                    // regista a hora de fim do processamento do pedido

            /* popula a mensagem que enviara para o processo de estatisticas */
            threadmsg.mtype = 1;
            threadmsg.thread_id = t_id;
            threadmsg.tipo_pedido = pedido->tipo_conteudo;
            strcpy(threadmsg.ficheiro, pedido->ficheiro);
            threadmsg.tempo_ini = time_inicio;
            threadmsg.tempo_fim = time_fim;

            /* envia mensagem */
            msgsnd(msgid, &threadmsg, sizeof(thread_msg) - sizeof(long), 0);        

            free(pedido);

        }
    

    }

    
    #ifdef DEBUG
    printf("[THREAD POOL %d] A terminar...\n", t_id);
    #endif
    

    return NULL;
}


void stats_manager() {
    FILE* fp;
    char file_path[FILE_PATH_BUF] = "";
    thread_msg threadmsg;
    
    struct tm info_time_inicio;
    struct tm info_time_fim;
 
    struct tm info_time_inicio_serv;
    struct tm info_time_fim_serv;
    


    
    #ifdef DEBUG
    printf("[STATS MANAGER] A iniciar servico de estatisticas...\n");
    #endif
    
    /* mapeia o espaco em memoria onde esta a informacao do servidor */
    servidor_info = (servidor*) shmat(shmem_id_servidor, NULL, 0);

    /* popula estrutura de tempo com a hora de arranque do servidor */
    time(&servidor_info->time_bootup);
    localtime_r(&servidor_info->time_bootup,&info_time_inicio_serv);
    
    /* configura o diretorio das estatisticas */
    sprintf(file_path, "%s", SERVER_PATH);
    strcat(file_path, "/stats.txt");
    
    

    fp = fopen(file_path, "w");                     // abre o ficheiro em permissao write
    fprintf(fp, "%s%20s%10s%11s%8s\n", "Tipo", "Ficheiro", "Thread", "Inicio", "Fim");      // cabecalho do documento
    fclose(fp);
    

    
    #ifdef DEBUG
    printf("[STATS MANAGER] Servico de estatisticas pronto.\n");
    #endif

    sem_post(&servidor_info->msgqueue_ready);


    while (testa_servidor_estado() == 1) {
        /* configura o diretorio das estatisticas */
        sprintf(file_path, "%s", SERVER_PATH);
        strcat(file_path, "/stats.txt");
        
        /* recebe mensagem da thread */
        msgrcv(msgid, &threadmsg, sizeof(thread_msg) - sizeof(long), 1, 0);
        
        /* se estado do servidor é encerramento/reinicio, verifica a condicao para sair do ciclo */
        if (testa_servidor_estado() < 1)
            continue;

        
        /* popula estruturas de tempo com os horarios de processamento do pedido vindo na mensagem da thread */
        localtime_r(&(threadmsg.tempo_ini), &info_time_inicio);
        localtime_r(&(threadmsg.tempo_fim), &info_time_fim);
        

        fp = fopen(file_path, "a");                 // abre documento para acrescentar dados

        /* armazena em documento, dados do processamento do peidido p pela thread t*/
        fprintf(fp, "%4c%20s%10d\t%02d:%02d\t%02d:%02d\n",
                threadmsg.tipo_pedido,
                threadmsg.ficheiro,
                threadmsg.thread_id,
                info_time_inicio.tm_hour,
                info_time_inicio.tm_min,
                info_time_fim.tm_hour,
                info_time_fim.tm_min);
        
        fclose(fp);



    }
    
    /* faz isto se servidor em modo de encerramento ou reiniciamento */
    if (testa_servidor_estado() < 1) {
        
        pthread_mutex_lock(&mutex_servidor_shmem);

        /* popula estrutura de tempo com a hora de encerramento */
        time(&servidor_info->time_shutdown);
        localtime_r(&servidor_info->time_shutdown,&info_time_fim_serv);


        /* imprime relatorio final */
        printf("\n\n==================================================\n");
        printf("RELATORIO FINAL DA SESSAO\n");
        printf("%-40s%02d:%02d\n", "Hora de arranque do servidor:", info_time_inicio_serv.tm_hour, info_time_inicio_serv.tm_min);
        printf("%-40s%02d:%02d\n", "Hora atual:", info_time_fim_serv.tm_hour, info_time_fim_serv.tm_min);
        printf("%-40s%d\n", "Numero total de acessos estaticos:", servidor_info->total_estaticos); 
        printf("%-40s%d\n", "Numero total de acessos dinamicos:", servidor_info->total_dinamicos);
        printf("%-40s%d\n", "Numero total de pedidos recusados:", servidor_info->total_recusados);
        printf("==================================================\n\n");

        /* faz reset a seguir a apresentacao do relatorio */
        servidor_info->total_dinamicos = 0;
        servidor_info->total_estaticos = 0;
        servidor_info->total_recusados = 0;
 
        
        
        pthread_mutex_unlock(&mutex_servidor_shmem);            
        
    }
    #ifdef DEBUG
    printf("[STATS MANAGER] A terminar centro de estatisticas...\n");
    #endif
    
    exit(0);
    
}




void init() {
    int i;
    /* inicializa as conexoes para o exterior */
    int port;                                           // porta do servidor usada para escutar
    int socket_ini;                                     // socket id antes de ligacao estabelicida
    struct sockaddr_in server_addr;                     // estrutura que define o no de ligacao no servidor
    
    thread_msg mensagem;

        #ifdef DEBUG
        printf("[SERVIDOR] A iniciar o servidor...\n");
        #endif

        sem_init(&sem_free_threads, 1, 0);               // cria semaforo com as threads da pool que estao desocupadas


        #ifdef DEBUG
        printf("[SERVIDOR] À espera das configuracoes...");
        #endif
        sem_wait(&config_info->sem_config);              // a espera de notificacao de processo de configuracoes a dizer que esta pronto
        #ifdef DEBUG
        printf("\n[SERVIDOR] Configuracoes disponiveis.\n");
        #endif

        #ifdef DEBUG
        printf("[SERVIDOR] À espera de centro de estatisticas...");
        #endif   
        sem_wait(&servidor_info->msgqueue_ready);       // a espera de notificacao de processo de estatisticas a dizer que esta pronto
        #ifdef DEBUG
        printf("\n[SERVIDOR] Centro de estatisticas inicializado com sucesso.\n");
        #endif



        /* INICIALIZACAO DE LIGACOES PARA O EXTERIOR */

        #ifdef DEBUG
        printf("[SERVIDOR] A configurar ligacoes para o exterior...\n");
        #endif

        /* mapeamento da memoria partilhada referente as configuracoes do servidores */
        config_info = (config*) shmat(shmem_id_config, NULL, 0);
        servidor_info = (servidor*) shmat(shmem_id_servidor, NULL, 0);


        /* acesso 'a estrutura das configuracoes do servidor localizada em memoria partilhada */
        pthread_mutex_lock(&mutex_config_shmem);            // fecha (se disponivel) o cadeado para aceder a memoria partilhada
        port = config_info->serv_port;                      // preenche o valor da porta com base no config file
        thread_pool_dim = config_info->thread_pool_dim;     // colocada em variavel global a dimensao da pool
        requests_buffer_dim = thread_pool_dim * 2;

        pthread_mutex_unlock(&mutex_config_shmem);          // volta a disponibilizar o cadeado
        /* fim de acesso a esturuta em memoria partilhada*/

        /* Preenchimento da estrutura do endereco do servidor */
        bzero((char *) &server_addr,sizeof(server_addr));   // inicializa todos os campos da estrutura a zeros, protegendo-os de valores lixo em memoria
        server_addr.sin_family = AF_INET;                   // dominio do endereco: AF_INET para Internet
        server_addr.sin_port = htons(port);                 // porta do servidor usada nas escutas (convertida de host byte order para network byte order)
        server_addr.sin_addr.s_addr = INADDR_ANY;           // endereco IP da maquina que hospeda o servidor fornecida pela constante INADDR_ANY


        /* criacao de um novo socket */
        socket_ini = socket(AF_INET, SOCK_STREAM, 0);       // AF_INET: domínio do endereco; SOCK_STREAM: stream continuo (orientado a TCP); 0: protocolo recomendado (TCP)
        if (socket_ini < 0) {                               // imprime mensagem de erro se houver falha a criar socket e termina o programa
            erro_fatal("ERRO a abrir um novo socket.");
        }


        /* evita problemas na reutilizacao de estruturas addr em utilizacao */
        /* configura o socket para tal efeito */
        int optval = 1;
        if ( setsockopt(socket_ini, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(int)) == -1 )
        {
            perror("setsockopt");
        }

        
        
        /* associa a estrutura de endereco do servidor com o novo socket , gera mensagem de erro se nao for possivel */
        if (bind(socket_ini, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0) {
            close(socket_ini);
            erro_fatal("ERRO na associacao do socket com estrutura addr. Socket pode estar ja a ser utizado.");
            exit(1);
        }

        /* escuta o dominio (Internet) na busca de novos clientes */
        if (listen(socket_ini, 5) < 0) {
            close(socket_ini);
            erro_fatal("ERRO no pedido listen() para o novo socket.");
            exit(1);
        }

        #ifdef DEBUG
        printf("[SERVIDOR] A escutar HTTP requests vindos da porta %d...\n", port);
        #endif

        /* pthreads - alocagem da pool (pthreads + id's) e criacao de todas as pthreads do programa */
        thread_pool = (pthread_t *) malloc(sizeof(pthread_t)*(thread_pool_dim));                // alocagem dinamica do espaco em memoria para a pool de threads
        thread_pool_id = (int *) malloc(thread_pool_dim);                                       // alocagem dinamica do espcao em memoria para id's da pool de threads

        pthread_create(&thread_receiver, NULL, receiver_main, &socket_ini);                     // redirecionamento da thread RECEIVER
        #ifdef DEBUG
        printf("[SERVIDOR] Thread Receiver criada.\n");
        #endif

        for (i = 0; i < thread_pool_dim; i++) {                                                 //redirecionamento de todas as threads na pool
            thread_pool_id[i] = i;
            pthread_create(thread_pool+i, NULL, pool_main, thread_pool_id+i);
            #ifdef DEBUG
            printf("[SERVIDOR] Thread (pool) #%d criada.\n", i);
            #endif
        }

        pthread_create(&thread_scheduler, NULL, scheduler_main, NULL);                          // redicecionamento da thread SCHEDULER
        #ifdef DEBUG
        printf("[SERVIDOR] Thread Scheduler criada.\n");
        #endif
        /* fim da inicializacao das pthreads */





        /* espera pela thread scheduler*/
        pthread_join(thread_scheduler, NULL);
        #ifdef DEBUG
        printf("[SERVIDOR] Thread Scheduler joined.\n");
        #endif

        /* Esperar por threads da pool */
        for (i = 0; i < thread_pool_dim; i++) {
            pthread_join(thread_pool[i], NULL);
            #ifdef DEBUG
            printf("[SERVIDOR] Thread #%d joined.\n", i);
            #endif
        }

        /* provocar o termino do gestor de estatisticas, enviando uma mensagem em branco para testar de novo a condicao*/
        msgsnd(msgid, &mensagem, sizeof(mensagem) - sizeof(long), 0);

        /* fecha de forma ordeira o socket */
        shutdown(socket_ini, SHUT_RDWR);
        close(socket_ini);

        /* espera pela thread receiver*/
        pthread_join(thread_receiver, NULL);

        #ifdef DEBUG
        printf("[SERVIDOR] Thread Receiver joined.\n");
        #endif

    exit(0);
    
}

void sighup(int signum) {
    #ifdef DEBUG
    printf("\n\n[SERVIDOR] PEDIDO DE CARREGAMENTO DE CONFIGURACOES DETETADO.\n");
    #endif

    pthread_mutex_lock(&mutex_servidor_shmem);
    servidor_info->servidor_on = 0;             // altera estado do servidor para modo de reiniciamento
    #ifdef DEBUG
    printf("\nSERVIDOR A ENCERRAR...\n\n");
    #endif
    pthread_mutex_unlock(&mutex_servidor_shmem);
    
}




void sigint(int signum) {
    #ifdef DEBUG
    printf("\n\n[SERVIDOR] PEDIDO DE ENCERRAMENTO DETETADO.\n");
    #endif
    
    // informacao que o servidor vai desligar
    pthread_mutex_lock(&mutex_servidor_shmem);
    servidor_info->servidor_on = -1;            // altera estado de servidor para modo de encerramento
    #ifdef DEBUG
    printf("\nSERVIDOR A ENCERRAR...\n\n");
    #endif
    pthread_mutex_unlock(&mutex_servidor_shmem);
    
}


int main(int argc, const char * argv[]) {
    int i;
    int parent_id = getpid();
    int pid;                                            // guarda valor retornado pelos fork()

    signal(SIGINT, sigint);                             // redirecionamento de todo o sinal sigint para a funcao correspondente
    signal(SIGHUP, sighup);                             // redirecionamento de todo o sinal sighup para a funcao correspondente
    
    sigset_t ignored_signals;                           // SET DE SINAIS IGNORADOS
    sigfillset(&ignored_signals);                       // preencher set com todos os sinais
    sigdelset(&ignored_signals, SIGINT);                // retirar SIGINT
    sigdelset(&ignored_signals, SIGHUP);                // retirar SIGHUP

    sigset_t handled_signals;                           // SET DE SINAIS APRENDIDOS
    sigemptyset (&handled_signals);                     // set vazio
    sigaddset (&handled_signals, SIGINT);               // adicionar SIGINT
    sigaddset (&handled_signals, SIGHUP);               // adicionar SIGHUP
    
    sigprocmask (SIG_SETMASK, &ignored_signals, NULL);  // ignorar todos os sinais deste set
    sigprocmask (SIG_BLOCK, &handled_signals, NULL);    // bloquear temporariamente o set de sinais ativos, de modo à zona critica seguinte correr sem interrupcoes


    /* criação e mapeação do espaco em memoria partilhada */
    shmem_id_config = shmget(IPC_PRIVATE, sizeof(config), IPC_CREAT|0700);
    config_info = (config*) shmat(shmem_id_config, NULL, 0);
    sem_init(&config_info->sem_config, 3, 0);           // inicia semaforo de configuracoes a 0, este processo fica a espera de receber post de processo de configuracoes
    
    shmem_id_servidor = shmget(IPC_PRIVATE, sizeof(servidor), IPC_CREAT|0700);
    servidor_info = (servidor*) shmat(shmem_id_servidor, NULL, 0);
    sem_init(&servidor_info->msgqueue_ready, 4, 0);     // inicia semaforo de estatisticas a 0, de forma a provocar blocking aqui enquanto tal processo nao esta pronto
    
    // informacao que o servidor esta ligado
    pthread_mutex_lock(&mutex_servidor_shmem);
    servidor_info->servidor_on = 1;
    pthread_mutex_unlock(&mutex_servidor_shmem);
    
    if ( (msgid = msgget(IPC_PRIVATE, IPC_CREAT|0700)) < 0)
        erro_fatal("Impossivel criar Fila de Mensagens");
    
    while(testa_servidor_estado() != -1 && getpid() == parent_id) {

        if(testa_servidor_estado() == 0) {
            #ifdef DEBUG
            printf("\nSERVIDOR A REINICIAR...\n\n");
            #endif
            sleep(10);
        }
        
        pthread_mutex_lock(&mutex_servidor_shmem);
        servidor_info->servidor_on = 1;                     // garante o estado de servidor em "ligado"
        pthread_mutex_unlock(&mutex_servidor_shmem);

        /* criacao de processos-filho */
        if ((pid = fork()) == 0) { 

            // processo-filho #1 - GESTOR DE CONFIGURACOES
            init();

        }

        else if (pid < 0) {
            erro_fatal("ERRO a criar processo-filho #1 - Gestor de Configuracoes");
        }

        else {
            if ((pid = fork()) == 0) {                              // processo-filho #2 - GESTOR DE ESTATISTICAS
                stats_manager();

            }

            else if (pid < 0) {
                erro_fatal("ERRO a criar processo-filho 2 - Gestor de Estatisticas");
            }

            else {
                if ((pid = fork()) == 0) {                          // processo-filho #3 - PRINCIPAL
                    configuration_manager();
                }

                else if (pid < 0) {
                    erro_fatal("ERRO a criar processo-filho 3 - Processo Principal");
                }
                else {
                    sigprocmask (SIG_UNBLOCK, &handled_signals, NULL);      // ativar sinais aprendidos a partir de agora

                     //Esperar por processos-filho
                    for (i = 0; i < 3; i++) {
                        wait (NULL);
                    }
                }

            }
        }
        
        
        /* fim da criacao de processos filho */

    }
    
    
    cleanup();                      // limpa/destroi estruturas em memoria partilhada
    

    return 0;
}
