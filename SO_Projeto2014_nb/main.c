//
//  main.c
//  SO_Projeto2014
//
//  Created by Miguel Diogo on 07/11/14.
//  Copyright (c) 2014 Miguel Diogo. All rights reserved.
//

#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
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

#define DEBUG           1                               // retirar para ocultar informacao de debug na consola

//#define SERVER_PATH     "/Users/MiguelDiogo/Documents/Faculdade/Ficheiros/Ano_2/Semestre_1/SO/Projeto/SO_Projeto2014/SO_Projeto2014/"
#define SERVER_PATH     "/mnt/hgfs/SO/Projeto/SO_Projeto2014/SO_Projeto2014/"

/* Modelos para HTTP Replies feitos pelo servidor */
#define HEADER_1        "HTTP/1.0 "                     // Primeira linha da resposta do servidor...
#define MESSAGE_200     "200 OK\r\n"                    // ... + Resposta se o pedido do cliente for aceite
#define MESSAGE_400     "400 Bad Request\r\n"
#define MESSAGE_401     "401 Unauthorized\r\n"
#define MESSAGE_404     "404 Not Found\r\n"
#define MESSAGE_500     "500 Internal Server Error\r\n"     // ... + Resposta se o pedido do cliente for recusado


#define SERVER_STRING   "Server: httpd/0.1.0\r\n"       // Nome do servidor
#define CONTENT_TYPE    "Content-type: "                // Tipo do conteudo/resposta

/* Modelos para HTTP Requests feitos pelo cliente */
#define GET_EXPR        "GET"                           // Expressão GET vinda dos clientes
#define CGI_EXPR        "cgi-bin/"                      // extensao do diretorio para acesso a conteudo dinamico
#define SIZE_BUF        1024                            // tamanho do buffer para pedidos e para respostas
#define SCRIPTS_MAX     10                              // numero de scripts maximo permitido




/**** VARIAVEIS GLOBAIS ***********************************************************************/



/* Estrutura partilhada populada pelas configuracoes do servidor */
struct c {
    int serv_port;                                      // porta do servidor usada para escutar
    int thread_pool_dim;                                // numero maximo de threads a serem usadas
    char sched_policy;                                   // politica de priorizacao do tipo de pedidos vindos do cliente
    char allowed_scripts[SCRIPTS_MAX][64];              // vetor populado com os scripts permitidos a pedido do cliente
    int allowed_scripts_num;                            // numero de scripts permitidos localizados no ficheiro de configuracao do servidor
};

typedef struct c config;

/* memoria partilhada */
int shmem_id_config;                                    // id da memoria partilhada da estrutura de configuracoes do servidor
int shmem_id_reqbuff;                                   // id da memoria partilhada da estrutura do array de pedidos

config* config_info = NULL;                              // ponteiro para estrutura de dados com as configuracoes do sistema



/* posix pthread variables */
pthread_mutex_t mutex_config_shmem = PTHREAD_MUTEX_INITIALIZER;     // mutex para as configuracoes do servidor em memoria partilhada
pthread_t thread_receiver;
pthread_t thread_scheduler;
pthread_t* thread_pool;                                 // ponteiro para espaco em memoria que aloca o conjunto de threads na pool
int* thread_pool_id;                                    // ponteiro para espaco em memoria que aloca o conjunto de id's das threads na pool
int thread_pool_dim;

/* variaveis partilhadas pelas threads 'Receiver' e 'Scheduler' */
char request_string[SIZE_BUF];                          // aqui e' armazenada de cada vez cada pedido vindo do exterior

/* variaveis acessiveis devidamente por mutex-exclusion *
 * logica Produtor/Consumidor                           *
 * Aplicado em: 'Requests buffer'(ver enunciado de proj */

pthread_mutex_t mutex_requests_shmem = PTHREAD_MUTEX_INITIALIZER;   // inicializacao de mutex para mutex-exclusion de dados acima
sem_t sem_reqs_buff_empty;                              // semaforo que controla os espacos livres no array de pedidos
sem_t sem_reqs_buff_full;                               // semaforo que controla os espacos ocupados no array de pedidos



/* REQUESTS*/

/* Estrutura molde para uma entrada no 'Request Buffer' */
typedef struct r {
    int socket;                                         // socket do cliente
    char ficheiro[256];                                 // ficheiro pedido pelo cliente
    int tipo_conteudo;                                  // tipo de conteudo: 0 -> estatico; 1 -> CGI; -1 -> Invalido
    int codigo_resposta;                                // resposta associada ao pedido (mensagens de erro ou sucesso devolvidas a cliente)
} Request;



typedef Request_node* Requests_list;                               // ponteiro para espaco de memoria criado para albergar todos os requests

typedef struct r {
    Request request_info;
    Requests_list next;
} Request_node;

Requests_list requests_buffer = NULL;

Request* request_alvo = NULL;                                              // variavel onde as threads recorrem para levantarem o pedido que lhes e' destinado
pthread_cond_t cond_requestalvo = PTHREAD_COND_INITIALIZER;
pthread_mutex_t mutex_requestalvo = PTHREAD_MUTEX_INITIALIZER; 


/* operacoes lista de requests*/

Requests_list inicia_requestList() {
    Requests_list requests_buffer;
    
    /* calculo do tamanho do 'requests_buffer' a partir do dobro de threads definidas para a pool (ver enunciado */
    int requests_buffer_size = thread_pool_dim * 2;
    
    shmem_id_reqbuff = shmget(IPC_PRIVATE, requests_buffer_size * sizeof(Request_node), IPC_CREAT|0700);   // array de requests, alocacao de memoria
    
    requests_buffer = (Requests_list) shmat(shmem_id_reqbuff, NULL, 0);                      // mapeacao do espaco de memoria partilhada correspondente
    requests_buffer->next = NULL;
    
}

void insere_request(Requests_list pedido) {
    
    Requests_list i;
    i = requests_buffer;
    
    sem_wait(&sem_reqs_buff_empty);
    pthread_mutex_lock(&mutex_requests_shmem);
    
    while(i->next != NULL) {
        i = i->next;
    }
    
    i->next = pedido;
    pedido->next = NULL;
    
    pthread_mutex_unlock(&mutex_requests_shmem);
    sem_post(&sem_reqs_buff_full);
    
      
}

int elimina_request(Requests_list pedido) {
    Requests_list ant = NULL;
    Requests_list atual = requests_buffer;
    int encontrado = 0;
    
    sem_wait(&sem_reqs_buff_full);
    pthread_mutex_lock(&mutex_requests_shmem);
    
    while(atual != NULL && encontrado == 0) {  
        ant = atual;
        atual = atual->next;
        
        if (atual->request_info.socket == pedido->request_info.socket &&
                atual->request_info.tipo_conteudo == pedido->request_info.tipo_conteudo &&
                strcmp(atual->request_info.ficheiro, pedido->request_info.ficheiro)==0) {
            encontrado = 1;

        }
        


    }
    
    if (encontrado == 1) {
        ant->next = atual->next;
        atual->next = NULL;

    }
    
    pthread_mutex_unlock(&mutex_requests_shmem);
    sem_post(&sem_reqs_buff_empty);
    
    
    return encontrado;   
    
}

Requests_list procura_pedido(int tipo_conteudo) {
    Requests_list ant = NULL;
    Requests_list atual = requests_buffer;
    int encontrado = 0;
    
    while(atual != NULL && encontrado == 0) {
        ant = atual;
        atual = atual->next;
        if (atual->request_info.tipo_conteudo == tipo_conteudo) {
            encontrado = 1;

        }
        
    }
    
    return atual;
    
}



/* FIM DE VARIAVEIS GLOBAIS *********************************************************************/



/* FEITO */
/* funcao que imprime erros fatais do programa */
void erro_fatal(char* mensagem) {
    perror(mensagem);
    exit(EXIT_FAILURE);
}

/* FEITO */
/* cria ou atualiza a estrutura armazenada em memoria partilhada que traduz o ficheiro de configuracao do servidor */
void configuration_manager() {
    char scripts_row[1024];                             // recebe todos os scripts permitidos, vindos do ficheiro diretamente
    char* token;                                        // percorre todos os scripts em "scripts_row"
    int i;
    char config_file_path[150] = "";
    strcat(strcat(config_file_path, SERVER_PATH), "config.txt");
    FILE* fp = fopen(config_file_path, "r");                // abre para leitura o ficheiro de configuracoes do servidor
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
    fscanf(fp, " sched_policy=%d", &config_info->sched_policy);
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
    
    exit(0);
}

/* FEITO */
void identifica(int socket_online, int* port_cliente, char* ipstr_cliente) {
    socklen_t socket_len;                               // dimensao do socket
    
    struct sockaddr_in * socket_addrin;                 // criacao de estrutura addr_in que vai interpretar a estruta addr_storage abaixo
    struct sockaddr_storage addr_storage;;              // criacao de estrutura addr_storage que contem o dominio do socket e a porta do cliente
    
    getpeername(socket_online, (struct sockaddr*) &addr_storage, &socket_len);              // interpretacao do socket_online que resulta em info da maquina do cliente
    
    /* assunmindo IPv4 */
    socket_addrin = (struct sockaddr_in *) &addr_storage;                                   // conversao do addr_storage em addr_in
    *port_cliente = ntohs(socket_addrin->sin_port);                                         // extracao da porta da estrutura para uma variavel
    inet_ntop(AF_INET, &socket_addrin->sin_addr, ipstr_cliente, sizeof(ipstr_cliente));     // extracao da string do ip da estrutura para uma variavel
}

/* FEITO */
int readline(int socket) {
    int i;
    char char_lido = '\0';                              // deposito de cada char da mensagem
    int end_of_file = 0;                                // 1 se EOF
    int nread = 0;                                // numero de caracteres lidos no total antes de mudanca de linha '\r'
   
    if ((nread = (int)read(socket, request_string, SIZE_BUF - 1)) == -1)
        nread = -1;
    
    else {
        request_string[nread] = '\0';
        *(strchr(request_string, '\r')) = '\0';

    }
    
    return nread;
}

/* FEITO */
int request_handler(int socket) {
    char comando[16];
    char full_filename[256];
    char filename[256];
    char formato[16];
    char versao_http[16];
    char* token;
    int content_analyser = 0;                                                           // 0: se estatico; 1 se CGI; -1 se nao suportado/acesso negado
    
    int requests_buffer_size = 0;
    
    
    Requests_list entry = (Requests_list*) malloc(sizeof(Request_node));                                //molde a ser preenchido pelo pedido e a ser entregue ao 'Request Buffer'
    
    entry->request_info.socket = socket;                                                             // guarda o socket da ligacao
    
   
    requests_buffer = inicia_requestList();                      // mapeacao do espaco de memoria partilhada correspondente
    
    
    sscanf(request_string, "%s %s %s", comando, full_filename, versao_http);            // extrai e analisa a string de pedido vinda do cliente
    
    
    sscanf(full_filename, "/cgi-bin/%s", filename);             // formato do pedido para CGI
    
    if (strcmp(filename, "") == 0)
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
            strcpy(entry->request_info.ficheiro, filename);                  // preenche o campo com o nome do ficheiro (ex: index.html)
            entry->request_info.tipo_conteudo = 0;                           // preenche o campo com o tipo de conteudo (ver legenda/comentario para a var 'content_analyser')
            entry->request_info.codigo_resposta = 200;

        }
        
        else if (strcmp(formato, "sh") == 0) {
            content_analyser = 1;                       // valor correspondente ao tipo de conteudo (1 para .sh)
            strcpy(entry->request_info.ficheiro, filename);          //  nome do ficheiro (ex: script1.sh)
            entry->request_info.tipo_conteudo = 0;                   // preenche o campo com o tipo de conteudo (ver legenda/comentario para a var 'content_analyser')
            entry->request_info.codigo_resposta = 200;

        }
        
        else {
            content_analyser = -1;                              // se nao é .html nem .sh -> valor de erro
            entry->request_info.codigo_resposta = 500;

        }
    }
    
    else {
        content_analyser = -1;                                  // se o cliente quer aceder a outros diretorios -> valor de erro
        entry->request_info.codigo_resposta = 500;
    }
        
    
    insere_request(entry);
    
    return content_analyser;
    
}

/* FEITO? */
void* receiver_main(void* socket_inicial) {
    int socket_online;                                  // novo socket id com ligacao estabelecida
    struct sockaddr_in client_addr;                     // estrutura que define o no de ligacao a ser preenchida pelo cliente recebido
    socklen_t client_addr_len = sizeof(client_addr);    // dimensao da estrutura que define o endereco do cliente
    
    int port_cliente;                                   // porta do cliente
    char ipstr_cliente[INET6_ADDRSTRLEN];               // cria array para alojar a string do ip do cliente
    
    while (1) {
        /* espera por ligacao a novo cliente */
        if ((socket_online = accept(*((int* )socket_inicial), (struct sockaddr *) &client_addr, &client_addr_len)) == -1) {
            erro_fatal("Erro a aceitar nova coneccao de novo cliente");
        }
        
        identifica(socket_online, &port_cliente, ipstr_cliente);    // identifica o IP e a porta do novo cliente
        
        #ifdef DEBUG
            printf("[RECEIVER] Conexao aceite vinda de %s:%d\n", ipstr_cliente, port_cliente);
        #endif
        
        readline(socket_online);                        // le primeira linha do pedido do cliente
        request_handler(socket_online);                 // analisa o pedido do cliente e formata-o
        
        
        /* TO DO **************************************************************
         *
         *
         * CONTINUAR???????????????????????
         * Parece acabado.
         *
         *
         */
        
        /*
         * Resto do pedido sera tratado pela thread 'Scheduler' e pela pool de Threads
         */
        
        
        close(socket_online);
    }
    
    return NULL;
}


void* scheduler_main() {
    int i;
                                                                // molde a ser preenchido pelo pedido e a ser entregue ao 'Request Buffer'
    char file_path[128] = SERVER_PATH;
    int valido = 0;                                             // 1 se valido
    FILE* fp;
    
    
    /* insere novo request no array 'Request_Buffer' */
    /* PRODUTOR DE REQUESTS (ver consumidor na thread do 'Scheduler') */
    if (config_info->sched_policy == 'e') {
        pthread_mutex_lock(&mutex_requests_shmem);              // mutex para a lista de requests
        pthread_mutex_lock(&mutex_requestalvo);                 // mutex para o requestalvo (parilhado por toda a pool)
        request_alvo = procura_pedido(0);                       // procura o proximo pedido de tipo t (0 para estatico, 1 par CGI)
        if (request_alvo != NULL) {       
            
            
            
            elimina_request(request_alvo);                      // elimina request da lista de requests
            pthread_cond_signal(&cond_requestalvo);             // sinaliza uma thread que tem o resquest_alvo agora esta preenchido
        }
        pthread_mutex_unlock(&mutex_requestalvo);               
        pthread_mutex_unlock(&mutex_requests_shmem);

    }
    
    if (request_alvo->tipo_conteudo == 0) {
        strcat(file_path, entry->request_info.ficheiro);
        if ((fp = fopen(file_path, "r")) != NULL) {
            valido = 1;
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
            entry->request_info.codigo_resposta = 404;
        }

        
    }
    else {
        pthread_mutex_lock(&mutex_config_shmem);
        /* ciclo percorre o array de scripts permitidos na estrutura partilhada */
        for (i = 0; i < config_info->allowed_scripts_num && valido == 0; i++) {
            /* se o script pedido se encontra no array de script permitidos carregado do ficheiro config e localizado em memoria partilhada */
            if (strcmp(entry->request_info.ficheiro, config_info->allowed_scripts[i]) == 0) {
                valido = 1;                        // preenche o campo com o tipo de conteudo (ver legenda/comentario para a var 'content_analyser')
                sprintf(entry->request_info.ficheiro, "%s%s", CGI_EXPR, entry->request_info.ficheiro);
                #ifdef DEBUG
                    printf("[Scheduler] Novo pedido: %s - Script Valido.\n", entry->request_info.ficheiro);
                #endif
                entry->request_info.codigo_resposta = 200;
                
            }
        }
        if (valido == 0) {
            #ifdef DEBUG
                printf("[Scheduler] Novo pedido: %s - Script Negado.\n", entry->request_info.ficheiro);
            #endif
            entry->request_info.codigo_resposta = 200;
        }
        
        pthread_mutex_unlock(&mutex_config_shmem);

    }
    
    if (valido == 1) {
        /* FALTA
         *
         * SCHEDULED THREADS
         *
         */
   
        
    }
        
  
    
    
    
    return NULL;
}


/* FEITO */
/* Envia pagina HTML para cliente */
void send_html_page(int threads_id, Requests_list entry, int socket) {
    FILE* fp;
    char line_buffer[SIZE_BUF];

    
    #ifdef DEFINE
    printf("[THREAD POOL %d] A enviar ficheiro HTML de %s para cliente...\n", t_id, entry->ficheiro);
    #endif
    
    while (fgets(line_buffer, SIZE_BUF, fp)) {
        send(socket, line_buffer, strlen(line_buffer), 0);
    }
    #ifdef DEFINE
    printf("[THREAD POOL %d] Ficheiro HTML de %s envio a cliente concluido\n", t_id, entry->ficheiro);
    #endif
    
    fclose(fp);
    
}

/* FALTA TRATAR CONTEUDO DINAMICO */
/* Funcao main de cada thread da pool para o acesso a I/O e envio de resposta a cliente */
void* pool_main(void* threads_id, Requests_list entry) {
    int t_id = *((int*)threads_id);
    char reply_buffer[SIZE_BUF];
    int socket = entry->request_info.socket;
    
    #ifdef DEBUG
    printf("[THREAD POOL %d] A enviar resposta ao pedido %s\n", t_id, entry->request_info.ficheiro);
    #endif
    
    /* Header */
    sprintf(reply_buffer, HEADER_1);
    send(socket, reply_buffer, strlen(HEADER_1), 0);
    
    /* Respostas consoante o sucesso da rececao do pedido */
    switch (entry->request_info.codigo_resposta) {
        case 200:
            sprintf(reply_buffer, MESSAGE_200);
            send(socket, reply_buffer, strlen(MESSAGE_200), 0);
            break;
        case 400:
            sprintf(reply_buffer, MESSAGE_400);
            send(socket, reply_buffer, strlen(MESSAGE_400), 0);
            break;
        case 401:
            sprintf(reply_buffer, MESSAGE_401);
            send(socket, reply_buffer, strlen(MESSAGE_401), 0);
            break;
        case 404:
            sprintf(reply_buffer, MESSAGE_404);
            send(socket, reply_buffer, strlen(MESSAGE_404), 0);
            break;
        case 500:
            sprintf(reply_buffer, MESSAGE_500);
            send(socket, reply_buffer, strlen(MESSAGE_500), 0);
            break;
            
        default:
            sprintf(reply_buffer, MESSAGE_500);
            send(socket, reply_buffer, strlen(MESSAGE_500), 0);
            break;
    }
    
    /* Informacao sobre o servidor e conteudo */
    sprintf(reply_buffer, SERVER_STRING);
    send(socket, reply_buffer, strlen(SERVER_STRING), 0);
    
    sprintf(reply_buffer, "%stext/html\r\n", CONTENT_TYPE);
    send(socket, reply_buffer, strlen(reply_buffer), 0);
    
    sprintf(reply_buffer, "\r\n");
    send(socket, reply_buffer, strlen(reply_buffer), 0);
    
    
    /* Pagina HTML retornada ao cliente quando pedido invalido */
    if (entry->request_info.codigo_resposta != 200) {
        sprintf(reply_buffer, "<HTML><TITLE>Not Found</TITLE>\r\n");
        send(socket, reply_buffer, strlen(reply_buffer), 0);
        sprintf(reply_buffer, "<BODY><P>Resource unavailable or nonexistent.\r\n");
        send(socket, reply_buffer, strlen(reply_buffer), 0);
        sprintf(reply_buffer, "</BODY></HTML>\r\n");
        send(socket, reply_buffer, strlen(reply_buffer), 0);
    }
    else {
        /* Conteudo estatico */
        if (entry->request_info.tipo_conteudo == 0) {
            send_html_page(t_id, entry, socket);
        }
        
        else {
            /* Conteudo dinamico */
            /* SCRIPT EXECUTE CHILD
             *
             *
             *
             *
             *
             *
             *
             */
            send_html_page(t_id, entry, socket);
        }
    }

    return NULL;
}

/* FEITO */
/* inicia as conexões do servidor*/
void warm_up() {
    int i;
    int port;                                           // porta do servidor usada para escutar
    int socket_ini;                                     // socket id antes de ligacao estabelicida
    struct sockaddr_in server_addr;                     // estrutura que define o no de ligacao no servidor
    
    /* mapeamento da memoria partilhada referente as configuracoes do servidores */
    config_info = (config*) shmat(shmem_id_config, NULL, 0);
    
    /* acesso 'a estrutura das configuracoes do servidor localizada em memoria partilhada */
    pthread_mutex_lock(&mutex_config_shmem);            // fecha (se disponivel) o cadeado para aceder a memoria partilhada
    port = config_info->serv_port;                      // preenche o valor da porta com base no config file
    thread_pool_dim = config_info->thread_pool_dim;     // colocada em variavel global a dimensao da pool
    
    pthread_mutex_unlock(&mutex_config_shmem);          // volta a disponibilizar o cadeado
    /* fim de acesso a esturuta em memoria partilhada*/
    printf("[INIT] porta: %d \n", port);
    
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
    
    
    /* associa a estrutura de endereco do servidor com o novo socket , gera mensagem de erro se nao for possivel */
    if (bind(socket_ini, (struct sockaddr *) &server_addr, sizeof(server_addr)) < 0)
        erro_fatal("ERRO na associacao do socket com estrutura addr. Socket pode estar ja a ser utizado.");
    
    /* escuta o dominio (Internet) na busca de novos clientes */
    if (listen(socket_ini, 5) < 0)
        erro_fatal("ERRO no pedido listen() para o novo socket.");
    
    #ifdef DEFINE
    printf("[SERVIDOR] A escutar HTTP requests vindos da porta %d...\n", port);
    #endif
    
    /* pthreads - alocagem da pool (pthreads + id's) e criacao de todas as pthreads do programa */
    thread_pool = (pthread_t *) malloc(sizeof(pthread_t)*(thread_pool_dim));   // alocagem dinamica do espaco em memoria para a pool de threads
    thread_pool_id = (int *) malloc(thread_pool_dim);                          // alocagem dinamica do espcao em memoria para id's da pool de threads
    
    pthread_create(&thread_receiver, NULL, receiver_main, &socket_ini);                     // redirecionamento da thread RECEIVER
    pthread_create(&thread_scheduler, NULL, scheduler_main, NULL);                          // redicecionamento da thread SCHEDULER
    
    for (i = 0; i < thread_pool_dim; i++) {                                                 //redirecionamento de todas as threads na pool
        pthread_create(thread_pool+i, NULL, pool_main, thread_pool_id+i);
    }
    /* fim da inicializacao das pthreads */
}

void init() {
    int i;

    /* inicializacao de semaforos usados na tread receiver e scheduler */
    sem_init(&sem_reqs_buff_empty, 2, thread_pool_dim * 2);
    sem_init(&sem_reqs_buff_full, 2, 0);
    
    /* inicializa as conexoes para o exterior */
    warm_up();
    
    /* Esperar por pthreads */
    for (i = 0; thread_pool_dim; i++) {
        pthread_join(thread_pool[i], NULL);
    }
    pthread_join(thread_receiver, NULL);
    pthread_join(thread_scheduler, NULL);
}

int main(int argc, const char * argv[]) {
    int i;
    int pid;                                            // guarda valor retornado pelos fork()
    /* TESTE **********************************************/
    //request_handler("GET /cgi-bin/script1.sh HTTP/1.1");
    /* APAGAR *********************************************/
    
    /* criação e mapeação do espaco em memoria partilhada */
    shmem_id_config = shmget(IPC_PRIVATE, sizeof(config), IPC_CREAT|0700);
    
    
    /* criacao de processos-filho */
    if ((pid = fork()) == 0) {                                  // processo-filho #1 - GESTOR DE CONFIGURACOES
        configuration_manager();
    }
    
    else if (pid < 0) {
        erro_fatal("ERRO a criar processo-filho #1 - Gestor de Configuracoes");
    }
    
    else {
        wait(NULL);                                             // espera que processo das configuracoes termine
        if ((pid = fork()) == 0) {                              // processo-filho #2 - GESTOR DE ESTATISTICAS
            // TO DO: STATISTICS MANAGER
            exit(0);

            
        }
        
        else if (pid < 0) {
            erro_fatal("ERRO a criar processo-filho 2 - Gestor de Estatisticas");
        }
        
        else {
            if ((pid = fork()) == 0) {                          // processo-filho #3 - PRINCIPAL
                init();
                exit(0);
            }
            
            else if (pid < 0) {
                erro_fatal("ERRO a criar processo-filho 3 - Processo Principal");
            }
            else {
                init();
            }

            
        }
    }
    /* fim da criacao de processos filho */


    
    /* Esperar por processos-filho */
    for (i = 0; i < 3; i++) {
        wait (NULL);
    }
    
    return 0;
}
