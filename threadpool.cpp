#include "threadpool.h"
#include <stdlib.h>
#include <semaphore.h>
#include <unordered_map>
#include <assert.h>


using namespace std;

thrdpool pool;
pthread_mutex_t poollock;              //给任务队列加锁
pthread_mutex_t vallock;                //给结果集合用的锁

sem_t tasksum;                                          //等待调度的任务数
sem_t trdsum;                                           //线程数

unordered_map <task_t, valnode * > valmap;            //结果集合


void *dotask(long t) {                                //执行任务
    void *retval;

    while (1) {
        pthread_mutex_lock(pool.lock + t);            //等待任务
        pthread_mutex_lock(&poollock);
        tasknode* node = pool.tsk[t];
        pthread_mutex_unlock(&poollock);
        retval = node->task(node->param);

        pthread_mutex_lock(&vallock);
        valnode * val=valmap.at(node->taskid);
        assert(val);
        assert(val->done == 0);
        assert(val->val == nullptr);
        if(val->waitc || (node->flags & NEEDRET)){
            val->done = 1;
            val->val = retval;         //存储结果
            pthread_cond_broadcast(&val->cond); //发信号告诉waittask
        }else{
            valmap.erase(node->taskid);
            pthread_cond_destroy(&val->cond);
            free(val);
        }
        pthread_mutex_unlock(&vallock);

        pthread_mutex_destroy(&node->lock);
        free(node);

        pthread_mutex_lock(&poollock);
        pool.tsk[t] = 0;
        pool.taskid[t] = 0;
        sem_post(&trdsum);
        pthread_mutex_unlock(&poollock);
    }

    return NULL;
}

//调度线程，按照先进先出的队列来调度
void sched() {
    int i;

    while (1) {
        sem_wait(&tasksum);                       //等待addtask的信号
        sem_wait(&trdsum);                        //等待一个空闲进程

        pthread_mutex_lock(&poollock);
        for (i = 0; i < pool.num; ++i) {
            if (pool.taskid[i] == 0)break;        //找到空闲进程号
        }
        pool.tsk[i] = pool.taskhead;                //分配任务
        pool.taskid[i] = pool.taskhead->taskid;
        pool.taskhead = pool.taskhead->next;        //把该任务从队列中取下
        pthread_mutex_unlock(&poollock);
        pthread_mutex_unlock(&pool.tsk[i]->lock);
        pthread_mutex_unlock(pool.lock + i);      //启动dotask
    }
}


void creatpool(int threadnum) {
    pool.num = threadnum;
    pool.curid = 1;
    pool.id = (pthread_t *)malloc(pool.num * sizeof(pthread_t));
    pool.lock = (pthread_mutex_t *)malloc(pool.num * sizeof(pthread_mutex_t));
    pool.tsk = (tasknode **)malloc(pool.num * sizeof(tasknode *));
    pool.taskhead = pool.tasktail = NULL;

    pool.taskid = (task_t *)malloc(pool.num * sizeof(task_t));
    sem_init(&tasksum, 0 , 0);
    sem_init(&trdsum, 0 , threadnum);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 20*1024*1024);               //设置20M的栈
    pthread_create(&pool.sched, &attr , (taskfunc)sched, NULL); //创建调度线程
    pthread_mutex_init(&poollock, NULL);
    pthread_mutex_init(&vallock, NULL);
    for (long i = 0; i < pool.num; ++i) {
        pthread_mutex_init(pool.lock + i, NULL);
        pthread_mutex_lock(pool.lock + i);                                    //让创建的线程全部暂停
        pool.taskid[i] = 0;
        pthread_create(pool.id + i, &attr, (taskfunc)dotask, (void *)i);
    }
    pthread_attr_destroy(&attr);
}


task_t addtask(taskfunc task, void *param , uint flags) {
    tasknode *t = (tasknode *)malloc(sizeof(tasknode));   //生成一个任务块
    t->param = param;
    t->task = task;
    t->next = NULL;
    t->flags = flags;
    pthread_mutex_init(&t->lock ,NULL);
    pthread_mutex_lock(&t->lock);

    valnode *val = (valnode *)malloc(sizeof(valnode));
    val->done = 0;
    val->waitc = 0;
    val->val = nullptr;
    val->cond=PTHREAD_COND_INITIALIZER;

    pthread_mutex_lock(&poollock);
    t->taskid = pool.curid++;
    if (pool.taskhead == NULL) {                              //加入任务队列尾部
        pool.taskhead = pool.tasktail = t;
    } else {
        pool.tasktail->next = t;
        pool.tasktail = t;
    }

    pthread_mutex_lock(&vallock);
    valmap[t->taskid] = val;
    assert(val);
    pthread_mutex_unlock(&vallock);
    
    pthread_mutex_unlock(&poollock);
    sem_post(&tasksum);                                       //发信号给调度线程
    if(flags & WAIT){
        pthread_mutex_lock(&t->lock);
    }
    return t->taskid;
}


//多线程同时查询，只保证最先返回的能得到结果，其他有可能返回NULL
void *waittask(task_t id) {
    void *retval = NULL;
    pthread_mutex_lock(&vallock);
    int count = valmap.count(id);

    if (count == 0) {                     //没有该任务或者已经取回结果，返回NULL
        pthread_mutex_unlock(&vallock);
        return NULL;
    }
    
    valnode *val = valmap.at(id);
    val->waitc++;
    while(val->done == 0){
        assert(val->val == nullptr);
        pthread_cond_wait(&val->cond, &vallock);        //等待任务结束
    }

    retval=val->val;
    val->waitc -- ;
    if(val->waitc==0){                                  //没有其他线程在等待结果，做清理操作
        pthread_cond_destroy(&val->cond); 
        free(val);
        valmap.erase(id); 
    }
    pthread_mutex_unlock(&vallock);
    return retval;
}


int taskisdoing(task_t id) {
    pthread_mutex_lock(&vallock);
    auto t = valmap.count(id);                     //没有该任务或者已经取回结果，返回0
    pthread_mutex_unlock(&vallock);
    return t;
}
