/**epoll test
author: gsf
date: 2017.07.31  
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <sys/types.h>
#include "threadpool.h"
#define IPADDRESS  "192.168.137.130"
//#define IPADDRESS   "127.0.0.1"
#define PORT        8787
#define MAXSIZE     512
#define LISTENQ     50 
#define FDSIZE      20000
#define EPOLLEVENTS 20000
#define EPOLL_LT 0
#define EPOLL_ET 1
#define FD_BLOCK 0
#define FD_NONBLOCK 1
//add task struct
typedef struct readinfo
{
	int epollfd;//epoll描述符
	int fd;//从哪个fd描述符读数据
	int flag;
}readinfo;

static int j=0;
//函数声明
//创建套接字并进行绑定
static int socket_bind(const char* ip,int port);
//IO多路复用epoll
static void do_epoll(int listenfd);
//事件处理函数
static void
handle_events(int epollfd,struct epoll_event *events,int num,int listenfd,char *buf);
//处理接收到的连接
static void handle_accpet(int epollfd,int listenfd);
//读处理
static void do_read(int epollfd,int fd,char *buf);
//写处理
static void do_write(int epollfd,int fd,char *buf);
int set_nonblock(int fd);
//添加事件
static void add_event(int epollfd,int fd,int state,int epoll_type,int block_type);
//修改事件
static void modify_event(int epollfd,int fd,int state,int epoll_type);
//删除事件
static void delete_event(int epollfd,int fd,int state,int epoll_type);
//add pool founction
/**************** 全局变量定义区 ***************/
 
thread_pool  	*g_pool = NULL;
thread_revoke 	*g_thread_revoke = NULL;
int				g_def_thread_num = 0;
int				g_manage_adjust_interval = 0;
int 			g_max_thread_num = 0;
int 			g_min_thread_num = 0;
int 			g_thread_worker_high_ratio = 0;
int 			g_thread_worker_low_ratio = 0;


/**	函数名：	get_config_value  int 的项值
   *	功能描述：	获取配置文件中某一项的值
   *	参数列表：  item:为配置文件中的项名
   *	返回值：	出错返回-1 成功返回项的值	 
   */
 int get_config_value(char *item)
{
	char value[50];
	if(GetParamValue(CONFIGFILENAME,item,value) == NULL)
	{
		return -1;
	}

	return atoi(value);
}


 /**	函数名：	get_config_value  int 的项值
   *	功能描述：	初始化配置文件项变量的值
   *	参数列表：  无
   *    返回值：	无	 
   */
void conf_init()
{
	g_max_thread_num = get_config_value(MAX_THREAD_NUM);
	g_min_thread_num = get_config_value(MIN_THREAD_NUM);
	g_def_thread_num = get_config_value(DEF_THREAD_NUM);
	g_manage_adjust_interval = get_config_value(MANAGE_ADJUST_INTERVAL);
	g_thread_worker_high_ratio = get_config_value(THREAD_WORKER_HIGH_RATIO);
	g_thread_worker_low_ratio = get_config_value(THREAD_WORKER_LOW_RATIO);

}

/**	函数名：	pool_init 
  * 功能描述：	初始化线程池
  *	参数列表：	max_thread_num :输入要建的线程池的线程最大数目
  *	返回值：	无
  */
void pool_init(int max_thread_num)
{
	int i;
	conf_init();
	if(max_thread_num < g_min_thread_num)
	{
		max_thread_num = g_min_thread_num;
	}
	else if(max_thread_num > g_max_thread_num)
	{
		max_thread_num = g_max_thread_num;
	}
	pthread_attr_t attr;
	int err;
	err= pthread_attr_init(&attr);
	if(err != 0)
	{
		perror("pthread_attr_init");
		exit(1);
	}
	err = pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);

	if(err != 0)
	{
		perror("pthread_attr_setdetachstate");
		exit(1);
	}

	g_pool = (thread_pool *)malloc(sizeof(thread_pool));
	
	pthread_mutex_init(&(g_pool->queue_lock),NULL);
	pthread_mutex_init(&(g_pool->remove_queue_lock),NULL);
	pthread_cond_init(&(g_pool->queue_ready),NULL);

	g_pool->queue_head = NULL;
	g_pool->max_thread_num = max_thread_num;
	g_pool->thread_queue =NULL;
	g_pool->thread_idle_queue = NULL;
	g_pool->idle_queue_num = 0;
	g_pool->cur_queue_size = 0;
	g_pool->shutdown = 0;

	int temp;
	for(i = 0; i < max_thread_num; i++)
	{
		pthread_t thread_id;
		pthread_create(&thread_id,&attr,thread_routine,NULL);
		thread_queue_add_node(&(g_pool->thread_queue),thread_id,&temp);
		printf("temp&&&&&&&&&&&&&%d\n",temp);//display thread num

	}
	
	pthread_attr_destroy(&attr);

}
/*
 线程取消函数
*/
void thread_revoke_init()
{
	g_thread_revoke = (thread_revoke *)malloc(sizeof(thread_revoke));
	
	pthread_mutex_init(&(g_thread_revoke->revoke_mutex),NULL);
	g_thread_revoke->thread_revoke_queue = NULL;
	g_thread_revoke->revoke_count = 0;
	g_thread_revoke->is_revoke = 0;
	g_thread_revoke->revoke_num = 0;
}
/**	函数名：	thread_queue_add_node

  * 功能描述：	向线程池中新增线程
  *	参数列表：	thread_queue:要增加的线程的线程队列 thread_id:线程的id
  *	返回值：	成功返回0 失败返回1
  */
int thread_queue_add_node(p_thread_queue_node *thread_queue,pthread_t thread_id,int *count)
{
	pthread_mutex_lock(&(g_pool->remove_queue_lock));
	//printf("++++count:%d++++++add thread id :%u++++\n",*count,thread_id);
	thread_queue_node *p = *thread_queue;
	thread_queue_node *new_node = (thread_queue_node *)malloc(sizeof(thread_queue_node));
	if(NULL == new_node)
	{
		printf("malloc for new thread queue node failed!\n");
		pthread_mutex_unlock(&(g_pool->remove_queue_lock));
		return 1;
	}
	
	new_node->thread_id = thread_id;
	new_node->next = NULL;
	
	/*如果队列为空*/
	if(NULL == *(thread_queue))
	{
		*(thread_queue) = new_node;
		(*count)++;		
		pthread_mutex_unlock(&(g_pool->remove_queue_lock));
		return 0;
	}
	
	/*每次都将新节点插入到队列头部*/
	new_node->next = p;
	*(thread_queue) = new_node;
	(*count)++;		
	pthread_mutex_unlock(&(g_pool->remove_queue_lock));
	return 0;
}
int thread_queue_remove_node(p_thread_queue_node *thread_queue,pthread_t thread_id,int *count)
{

	pthread_mutex_lock(&(g_pool->remove_queue_lock));
	printf("---count:%d------remove threadid : %u----\n",*count,thread_id);
	p_thread_queue_node current_node,pre_node;
	if(NULL == *(thread_queue))
	{
		printf("revoke a thread node from queue failed!\n");
		pthread_mutex_unlock(&(g_pool->remove_queue_lock));
		return 1;
	}

	current_node = *(thread_queue);
	pre_node = *(thread_queue);
	int i = 1;
	while(i < g_pool->max_thread_num && current_node != NULL)
	{
		printf("i = %d, max_thread_num = %d \n",i,g_pool->max_thread_num);
		i++;
		if(thread_id == current_node->thread_id)
		{
			break;
		}
		pre_node = current_node;
		current_node = current_node->next;
	
	}
	
	if(NULL == current_node)
	{
		printf("revoke a thread node from queue failed!\n");
		pthread_mutex_unlock(&(g_pool->remove_queue_lock));
		return 1;
	}

	/*找到该线程的位置，删除对应的线程节点 如果要删除的节点就是头节点 */
	if(current_node->thread_id == (*(thread_queue))->thread_id)
	{
		*(thread_queue) = (*(thread_queue))->next;
		free(current_node);
		(*count)--;
		pthread_mutex_unlock(&(g_pool->remove_queue_lock));
		return 0;
	}

	/*找到该线程的位置，删除对应的线程节点 如果要删除的节点就是尾节点 */
	if(current_node->next == NULL)
	{
		pre_node->next =NULL;
		free(current_node);
		(*count)--;
		pthread_mutex_unlock(&(g_pool->remove_queue_lock));
		return 0;
	}
	pre_node = current_node->next;
	free(current_node);
	(*count)--;
	printf("0 max_thread_num = %d\n",g_pool->max_thread_num);
	pthread_mutex_unlock(&(g_pool->remove_queue_lock));
	return 0;
}


/**	函数名：	pool_add_worker 
  * 功能描述：	向线程池中加任务
  *	参数列表：	process :函数指针，指向处理函数用作真正的工作处理
  *				arg:工作队列中的参数
  *	返回值：	成功返回0,失败返回-1
  */
int pool_add_worker(void*(*process)(void *arg),void *arg)
{
	thread_worker *new_work = (thread_worker *)malloc(sizeof(thread_worker));
	
	if(new_work == NULL)
	{
		return -1;
	}
	new_work->process = process;
	new_work->arg = arg;
	new_work->next = NULL;

	pthread_mutex_lock(&(g_pool->queue_lock));

	/*将任务加入等待队列中*/
	thread_worker *member = g_pool->queue_head;
	if(member != NULL)
	{
		while(member->next != NULL)
		{
			member = member->next;
		}

		member->next = new_work;
	}
	else
	{
		g_pool->queue_head = new_work;
	}

	assert(g_pool->queue_head != NULL);

	g_pool->cur_queue_size++;
	pthread_mutex_unlock(&(g_pool->queue_lock));
//	printf("add task to pool\n");
	/*等待队列中有新任务了，唤醒一个等待线程处理任务；注意，如果所有的线程都在忙碌，这句话没有任何作用*/
	pthread_cond_signal(&(g_pool->queue_ready));

	return 0;
}

/**	函数名：	pool_add_thread 

  * 功能描述：	向线程池中新增线程
  *	参数列表：	thread_num:要增加的线程数目
  *	返回值：	无
  */
void pool_add_thread(int thread_num)
{
	int i;
	pthread_attr_t attr;
	int err = pthread_attr_init(&attr);
	if(err != 0)
	{
		perror("pthread_attr_init");
		exit(1);
	}
	err = pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);

	if(err != 0)
	{
		perror("pthread_attr_setdetachstate");
		exit(1);
	}
	
	for(i = 0; i < thread_num; i++)
	{
		pthread_t thread_id;
		pthread_create(&thread_id,&attr,thread_routine,NULL);
		thread_queue_add_node(&(g_pool->thread_queue),thread_id,&(g_pool->max_thread_num));
	}

	pthread_attr_destroy(&attr);
}

/**	函数名：	pool_revoke_thread
  *     功能描述：	从线程池线程中撤销线程
  *	参数列表：	thread_num:要撤销的线程数目
  *	返回值：	无
  */
void pool_revoke_thread(int thread_num)
{
	if(thread_num == 0)
	{
		return;
	}

	g_thread_revoke->revoke_num =thread_num;
	g_thread_revoke->is_revoke = 1;
	printf("----max_thread_num %d---------revoke %d thread-----\n",g_pool->max_thread_num,thread_num);
	thread_queue_node * p = g_thread_revoke->thread_revoke_queue;

	pthread_cond_broadcast(&(g_pool->queue_ready));

}

void * thread_manage(void *arg)
{
	int optvalue;
	int thread_num;
	while(1)
	 {
	 
		 if(g_pool->cur_queue_size > g_thread_worker_high_ratio * g_pool->max_thread_num)
	 	{
			 optvalue = 1;
			 thread_num =(g_pool->cur_queue_size -g_thread_worker_high_ratio * g_pool->max_thread_num) / g_thread_worker_high_ratio;
		}
	 	else if (g_pool->cur_queue_size * g_thread_worker_low_ratio < g_pool->max_thread_num)
	 	{
			optvalue = 2;
			thread_num =(g_pool->max_thread_num -g_thread_worker_low_ratio * g_pool->cur_queue_size) / g_thread_worker_low_ratio;
	 	}
	 
		if(1 == optvalue)
	 	{
			
			if(g_pool->max_thread_num + thread_num > g_max_thread_num)
			{
				thread_num = g_max_thread_num - g_pool->max_thread_num;
			}
			pool_add_thread(thread_num);
			
	 	}
		else if( 2 == optvalue)
		{
			if(g_pool->max_thread_num - thread_num < g_min_thread_num)
			{
				thread_num = g_pool->max_thread_num - g_min_thread_num;
			}
		//	pthread_t revoke_tid;
		//	pthread_create(&revoke_tid,NULL,(void *)pool_revoke_thread,(void *)thread_num);
			pool_revoke_thread(thread_num);
	 	}

//		printf("==========ManageThread=============\n");

		printf("cur_queue_size = %d  |  max_thread_num = %d\n",g_pool->cur_queue_size,g_pool->max_thread_num);
		conf_init();
		sleep(g_manage_adjust_interval);
 }



	
	
}


/**	函数名：	pool_destroy 
  *     功能描述：	销毁线程池
  *	参数列表：	无
  *	返回值：	成功返回0,失败返回-1
  */
int pool_destroy()
{
	if(g_pool->shutdown)
	{
		return -1;
	}
	
	g_pool->shutdown = 1;

	/* 唤醒所有等待线程，线程池要销毁  */
	pthread_cond_broadcast(&(g_pool->queue_ready));

	/* 阻塞等待线程退出，防止成为僵尸  */
	thread_queue_node * q = g_pool->thread_queue;
	thread_queue_node * p = q;
	sleep(10);
	g_pool->thread_queue = NULL;

	/* 销毁等待队列  */
	thread_worker *head = NULL;

	while(g_pool->queue_head != NULL)
	{
		head = g_pool->queue_head;
		g_pool->queue_head = g_pool->queue_head->next;
		free(head);
	}
	
	g_pool->queue_head = NULL;

	/* 条件变量和互斥量销毁  */
	pthread_mutex_destroy(&(g_pool->queue_lock));
	pthread_mutex_destroy(&(g_pool->remove_queue_lock));
	pthread_cond_destroy(&(g_pool->queue_ready));
	
	/* 销毁整个线程池  */
	free(g_pool);
	g_pool = NULL;

	return 0;
	
}

void cleanup(void *arg)
{
		thread_queue_remove_node(&(g_pool->thread_queue),pthread_self(),&(g_pool->max_thread_num));
		pthread_mutex_unlock(&(g_pool->queue_lock));
//		printf("thread ID %d will exit\n",pthread_self());
	
}

/**	函数名：	thread_routine 
  * 功能描述：	线程池中的线程
  *	参数列表：	arg 线程附带参数 一般为NULL；
  *	返回值：   
  */
void * thread_routine(void *arg)
{
//	printf("starting thread ID:%u\n",pthread_self());
	while(1)
	{
		pthread_mutex_lock(&(g_pool->queue_lock));
		/* 如果等待队列为0 并且不销毁线程池，则处于阻塞状态
		 *pthread_cond_wait 是原子操作，等待前解锁，唤醒后加锁
		 */

		while(g_pool->cur_queue_size == 0 && !g_pool->shutdown )
		{
//			printf("thread ID %u is waiting \n",pthread_self());
			pthread_cond_wait(&(g_pool->queue_ready),&(g_pool->queue_lock));
		}

		/* 如果线程池要销毁 */
		if(g_pool->shutdown)
		{
			thread_queue_remove_node(&(g_pool->thread_queue),pthread_self(),&(g_pool->max_thread_num));
			pthread_mutex_unlock(&(g_pool->queue_lock));
			printf("thread ID %d will exit\n",pthread_self());
			pthread_exit(NULL);
		}

		if(g_thread_revoke->is_revoke != 0 && g_thread_revoke->revoke_count < g_thread_revoke->revoke_num)
		{
		/*	if(g_thread_revoke->revoke_count >= g_thread_revoke->revoke_num )
			{
				
			printf("-revoke-@@jie锁@@+++\n");	
				pthread_mutex_unlock(&(g_pool->queue_lock));
				continue;
			}*/
			thread_queue_remove_node(&(g_pool->thread_queue),pthread_self(),&(g_pool->max_thread_num));
		
			thread_queue_add_node(&(g_thread_revoke->thread_revoke_queue),pthread_self(),&(g_thread_revoke->revoke_count));
			g_thread_revoke->revoke_count++;
			pthread_mutex_unlock(&(g_pool->queue_lock));
			printf("revoke success thread ID %d will exit\n",pthread_self());
			pthread_exit(NULL);

		}
//		printf("the task queue size is :%d\n",g_pool->cur_queue_size);
//		printf("thread ID %u is starting to work\n",pthread_self());
		assert(g_pool->cur_queue_size != 0);
		assert(g_pool->queue_head != NULL);

		/* 等待队列长度减1，并且取出链表的头元素  */
		g_pool->cur_queue_size--;
		thread_worker * worker = g_pool->queue_head;
		g_pool->queue_head = worker->next;
		pthread_mutex_unlock(&(g_pool->queue_lock));
//		printf("＊＊＊＊＊＊＊＊＊＊＊＊执行任务\n");
		/* 调用回调函数，执行任务  */
		(*(worker->process))(worker->arg);
		free(worker);
		worker = NULL;
			
	}
	pthread_exit(NULL);
}
void *myprocess(void *arg)
{
    int nread;
    int epollfd=((readinfo*)arg)->epollfd;
    int fd=((readinfo*)arg)->fd;
    int flag=((readinfo*)arg)->flag;
    free((readinfo*)arg);
    char buf[100];
//   printf("thread ID is %u, working on task%d\n",pthread_self(),fd);
   if(flag==0)// 0 is read
   { 
	memset(buf,0,100*sizeof(char));
	while(1)
	{
		nread=read(fd,buf,100);
		if(nread==-1)
		{
			if(errno == EAGAIN || errno == EWOULDBLOCK)
			{//	printf("循环读取所有数据\n");
				modify_event(epollfd,fd,EPOLLOUT,EPOLL_ET);
				break;
			}
			//else if(errno==EINTR)
			//	continue;
			else	
			{//出错了，所以关闭并且break跳出循环返回；
				delete_event(epollfd,fd,EPOLLIN,EPOLL_ET);
				close(fd);break;
			}
		}
		else if(nread==0)	
		{
			printf("client close\n");
			close(fd);
			delete_event(epollfd,fd,EPOLLIN,EPOLL_ET);
			break;
		}
		else	
		{
	//		 printf("read message is : %s",buf);
		}
	}
    }
    else // 1 is send
    {
        int nwrite;
	sprintf(buf,"welcome to my test server\n");
    	int nleft=strlen(buf);
	int nwritepos=0;
	/*nwrite = write(fd,buf,strlen(buf));
    	if (nwrite == -1)
    	{
        	perror("write error:");
        	close(fd);
        	delete_event(epollfd,fd,EPOLLOUT);
    	}
    	else
        	modify_event(epollfd,fd,EPOLLIN);
    	memset(buf,0,100);*/
	while(nleft>0)	
	{
		nwrite=write(fd,buf+nwritepos,nleft);
		if(nwrite==0)
		{
			perror("write error:");
			close(fd);
			delete_event(epollfd,fd,EPOLLOUT,EPOLL_ET);
		}
		if(nwrite==-1)
		{
			if(errno == EWOULDBLOCK || errno == EAGAIN)	
				nwrite = 0;
			else if(errno == EINTR)
				continue;
            		else
           		{
				printf("nwrite -1 error\n");
                		close(fd);
	                        delete_event(epollfd,fd,EPOLLOUT,EPOLL_ET);
            		}
		}
		else
		{
			nleft-=nwrite;
			nwritepos+=nwrite;
		}
	}
	modify_event(epollfd,fd,EPOLLIN,EPOLL_ET);
    }
    return NULL;
}
int main(int argc,char *argv[])
{
    int  listenfd;
    listenfd = socket_bind(IPADDRESS,PORT);
    listen(listenfd,LISTENQ);
    //add pool init
    pthread_t manage_tid;
    thread_revoke_init(); 	
    sleep(1);
//    conf_init();
    pool_init(g_def_thread_num);
    sleep(3);
    pthread_create(&manage_tid,NULL,thread_manage,NULL);
    do_epoll(listenfd);

    return 0;
}

static int socket_bind(const char* ip,int port)
{
    int  listenfd;
    struct sockaddr_in servaddr;
    listenfd = socket(AF_INET,SOCK_STREAM,0);
    if (listenfd == -1)
    {
        perror("socket error:");
        exit(1);
    }
    int opt=1;
    setsockopt(listenfd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
    bzero(&servaddr,sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    inet_pton(AF_INET,ip,&servaddr.sin_addr);
    servaddr.sin_port = htons(port);
    if (bind(listenfd,(struct sockaddr*)&servaddr,sizeof(servaddr)) == -1)
    {
        perror("bind error: ");
        exit(1);
    }
    return listenfd;
}

static void do_epoll(int listenfd)
{
    int epollfd;
    struct epoll_event events[EPOLLEVENTS];
    int ret;
    char buf[MAXSIZE];
    memset(buf,0,MAXSIZE);
    epollfd = epoll_create(FDSIZE);
    //添加监听描述符事件
    add_event(epollfd,listenfd,EPOLLIN,EPOLL_LT,FD_NONBLOCK);
    int i=0;
    for (i=0 ; ;i++ )
    {
	
        ret = epoll_wait(epollfd,events,EPOLLEVENTS,-1);
        handle_events(epollfd,events,ret,listenfd,buf);
    }
    close(epollfd);
    pool_destroy();
}

static void
handle_events(int epollfd,struct epoll_event *events,int num,int listenfd,char *buf)
{
    int i;
    int fd;
   // int j=0;
    //进行选好遍历
    for (i = 0;i < num;i++)
    {
        fd = events[i].data.fd;
        //根据描述符的类型和事件类型进行处理
        if ((fd == listenfd) &&(events[i].events & EPOLLIN))
            handle_accpet(epollfd,listenfd);
        else if (events[i].events & EPOLLIN)
	{
		readinfo  *readmsg=(readinfo*)malloc(sizeof(readinfo));
		readmsg->epollfd=epollfd;
		readmsg->fd=fd;
		readmsg->flag=0;//0 is read			
		pool_add_worker(myprocess,readmsg);
	}
        else if (events[i].events & EPOLLOUT)
	{
		readinfo *writemsg=(readinfo*)malloc(sizeof(readinfo));
		writemsg->epollfd=epollfd;
		writemsg->fd=fd;
		writemsg->flag=1;
                pool_add_worker(myprocess,writemsg);
	}
    }
}
static void handle_accpet(int epollfd,int listenfd)
{
    int clifd;
    struct sockaddr_in cliaddr;
   // socklen_t  cliaddrlen;
	int cliaddrlen=sizeof(cliaddr);
    clifd = accept(listenfd,(struct sockaddr*)&cliaddr,&cliaddrlen);
    if (clifd == -1)
        perror("accpet error:");
    else
    {
//        printf("accept a new client: %s:%d\n",inet_ntoa(cliaddr.sin_addr),cliaddr.sin_port);
        //添加一个客户描述符和事件
       // add_event(epollfd,clifd,EPOLLIN);
	//set the socket is ET and nonblock
	add_event(epollfd,clifd,EPOLLIN,EPOLL_ET,FD_NONBLOCK);
    }
}

static void do_read(int epollfd,int fd,char* buf)
{
    int nread;
    char bufread[200];
    memset(bufread,0,200*sizeof(char));
    nread = read(fd,bufread,200);
    if (nread == -1)
    {
        //perror("read error:");
        close(fd);
        delete_event(epollfd,fd,EPOLLIN,EPOLL_ET);
    }
    else if (nread == 0)
    {
        //fprintf(stderr,"client close.\n");
	printf("client close\n");
        close(fd);
        delete_event(epollfd,fd,EPOLLIN,EPOLL_ET);
    }
    else
    {
        printf("read message is : %s\n",bufread);
        //修改描述符对应的事件，由读改为写
        modify_event(epollfd,fd,EPOLLOUT,EPOLL_ET);
    }
}

static void do_write(int epollfd,int fd,char *buf)
{
    int nwrite;
    char buf1[]={"welcome to my server"};
    nwrite = write(fd,buf1,strlen(buf1));
    if (nwrite == -1)
    {
        perror("write error:");
        close(fd);
        delete_event(epollfd,fd,EPOLLOUT,EPOLL_ET);
    }
    else
        modify_event(epollfd,fd,EPOLLIN,EPOLL_ET);
    memset(buf,0,MAXSIZE);
}

static void add_event(int epollfd,int fd,int state,int epoll_type,int block_type)
{
    struct epoll_event ev;
    ev.events = state;//因为一般都是需要监听套接字的输入情况
    ev.data.fd = fd;
    /* 如果是ET模式，设置EPOLLET */
    if (epoll_type == EPOLL_ET)//如果需要将该套结字对应的输入情况设定为边缘触发
    	ev.events |= EPOLLET;
    if(block_type==FD_NONBLOCK)
   	set_nonblock(fd);
    epoll_ctl(epollfd,EPOLL_CTL_ADD,fd,&ev);
}

static void delete_event(int epollfd,int fd,int state,int epoll_type)
{
    struct epoll_event ev;
    ev.events = state;
    ev.data.fd = fd;
    if(epoll_type==EPOLL_ET)
	ev.events|=EPOLLET;
    epoll_ctl(epollfd,EPOLL_CTL_DEL,fd,&ev);
}

static void modify_event(int epollfd,int fd,int state,int epoll_type)
{
    struct epoll_event ev;
    ev.events = state;
    ev.data.fd = fd;
    if(epoll_type==EPOLL_ET)
	ev.events|=EPOLLET;
    epoll_ctl(epollfd,EPOLL_CTL_MOD,fd,&ev);
}

/* 设置文件为非阻塞 */
 int set_nonblock(int fd)
 {
      int old_flags = fcntl(fd, F_GETFL);
      fcntl(fd, F_SETFL, old_flags | O_NONBLOCK);
      return old_flags;
 }

