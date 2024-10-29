#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/semaphore.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/atomic.h>

static int prod = 1;
static int cons = 0;
static int size = 10;
static int uid = 0;
static int exit_flag = 0;
static atomic_t active_consumers;

module_param(prod, int, 0);
module_param(cons, int, 0);
module_param(size, int, 0);
module_param(uid, int, 0);

static struct semaphore empty;
static struct semaphore full;
static struct task_struct **zombie_buffer;
static struct task_struct **consumer_threads;
static int in = 0;
static int out = 0;
static struct task_struct *producer_thread;
static DEFINE_MUTEX(buffer_mutex);

#define EXIT_ZOMBIE 0x00000020

int producer_function(void *data) {
    struct task_struct *p;
    
    while (!kthread_should_stop() && !exit_flag) {
        for_each_process(p) {
            if (p->cred->uid.val == uid && (p->exit_state & EXIT_ZOMBIE)) {
                if (down_interruptible(&empty))
                    continue;
                    
                mutex_lock(&buffer_mutex);
                zombie_buffer[in] = p;
                in = (in + 1) % size;
                mutex_unlock(&buffer_mutex);
                
                up(&full);
            }
        }
        msleep(100);
    }
    return 0;
}

int consumer_function(void *data) {
    int id = *(int *)data;
    struct task_struct *zombie;
    kfree(data);

    atomic_inc(&active_consumers);

    while (!kthread_should_stop() && !exit_flag) {
        if (down_interruptible(&full)) {
            break;
        }

        if (kthread_should_stop() || exit_flag) {
            up(&full);
            break;
        }

        mutex_lock(&buffer_mutex);
        zombie = zombie_buffer[out];
        if (zombie) {
            kill_pid(zombie->real_parent->thread_pid, SIGKILL, 0);
            out = (out + 1) % size;
        }
        mutex_unlock(&buffer_mutex);
        
        up(&empty);
        msleep(50);
    }

    atomic_dec(&active_consumers);
    return 0;
}

static int __init zombie_killer_init(void) {
    int i;

    if (size <= 0) {
        printk(KERN_ERR "Invalid buffer size\n");
        return -EINVAL;
    }

    atomic_set(&active_consumers, 0);

    zombie_buffer = kmalloc_array(size, sizeof(struct task_struct *), GFP_KERNEL);
    if (!zombie_buffer)
        return -ENOMEM;

    consumer_threads = kmalloc_array(cons, sizeof(struct task_struct *), GFP_KERNEL);
    if (!consumer_threads) {
        kfree(zombie_buffer);
        return -ENOMEM;
    }

    for (i = 0; i < size; i++)
        zombie_buffer[i] = NULL;

    sema_init(&empty, size);
    sema_init(&full, 0);

    producer_thread = kthread_run(producer_function, NULL, "producer");
    if (IS_ERR(producer_thread)) {
        kfree(zombie_buffer);
        kfree(consumer_threads);
        return PTR_ERR(producer_thread);
    }

    for (i = 0; i < cons; i++) {
        int *id = kmalloc(sizeof(int), GFP_KERNEL);
        if (!id) {
            exit_flag = 1;
            kthread_stop(producer_thread);
            while (--i >= 0)
                kthread_stop(consumer_threads[i]);
            kfree(zombie_buffer);
            kfree(consumer_threads);
            return -ENOMEM;
        }
        *id = i + 1;
        
        consumer_threads[i] = kthread_run(consumer_function, id, "consumer-%d", i);
        if (IS_ERR(consumer_threads[i])) {
            kfree(id);
            exit_flag = 1;
            kthread_stop(producer_thread);
            while (--i >= 0)
                kthread_stop(consumer_threads[i]);
            kfree(zombie_buffer);
            kfree(consumer_threads);
            return PTR_ERR(consumer_threads[i]);
        }
    }

    return 0;
}

static void __exit zombie_killer_exit(void) {
    int i;
    exit_flag = 1;
    msleep(200);

    if (producer_thread) {
        kthread_stop(producer_thread);
        msleep(100);
    }

    for (i = 0; i < cons; i++) {
        up(&full);
        up(&empty);
    }

    for (i = 0; i < cons; i++) {
        if (consumer_threads[i]) {
            kthread_stop(consumer_threads[i]);
        }
    }

    // Wait for all consumers to finish
    while (atomic_read(&active_consumers) > 0) {
        msleep(50);
    }

    mutex_lock(&buffer_mutex);
    kfree(zombie_buffer);
    kfree(consumer_threads);
    mutex_unlock(&buffer_mutex);
}

module_init(zombie_killer_init);
module_exit(zombie_killer_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Vamsi Krishna Vasa");
MODULE_DESCRIPTION("Zombie Killer: A module to identify and kill zombie processes.");
