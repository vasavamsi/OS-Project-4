#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/semaphore.h>
#include <linux/sched/signal.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("Zombie Killer Kernel Module");

// Module parameters
static int prod = 1;
static int cons = 1;
static int size = 1;
static int uid = 0;

module_param(prod, int, 0644);
module_param(cons, int, 0644);
module_param(size, int, 0644);
module_param(uid, int, 0644);

// Shared buffer structure
struct buffer {
    struct task_struct **items;
    int in;
    int out;
    int count;
    struct semaphore empty;
    struct semaphore full;
    struct semaphore mutex;
};

static struct buffer *shared_buffer;
static struct task_struct **producer_thread;
static struct task_struct **consumer_threads;
static int should_stop = 0;

// Producer thread function
static int producer_fn(void *data)
{
    struct task_struct *task;
    
    while (!kthread_should_stop()) {
        rcu_read_lock();
        for_each_process(task) {
            if (task->real_cred->uid.val == uid && 
                task->exit_state == EXIT_ZOMBIE) {
                
                down(&shared_buffer->empty);
                down(&shared_buffer->mutex);
                
                shared_buffer->items[shared_buffer->in] = task;
                shared_buffer->in = (shared_buffer->in + 1) % size;
                shared_buffer->count++;
                
                printk(KERN_INFO "[Producer-%d] has produced a zombie process with pid %d and parent pid %d\n",
                       current->pid, task->pid, task->real_parent->pid);
                
                up(&shared_buffer->mutex);
                up(&shared_buffer->full);
            }
        }
        rcu_read_unlock();
        msleep(250);
    }
    return 0;
}

// Consumer thread function
static int consumer_fn(void *data)
{
    struct task_struct *zombie;
    struct pid *pid_struct;
    
    while (!kthread_should_stop()) {
        down(&shared_buffer->full);
        down(&shared_buffer->mutex);
        
        if (shared_buffer->count > 0) {
            zombie = shared_buffer->items[shared_buffer->out];
            shared_buffer->out = (shared_buffer->out + 1) % size;
            shared_buffer->count--;
            
            pid_struct = find_get_pid(zombie->real_parent->pid);
            if (pid_struct) {
                kill_pid(pid_struct, SIGKILL, 1);
                put_pid(pid_struct);
                
                printk(KERN_INFO "[Consumer-%d] has consumed a zombie process with pid %d and parent pid %d\n",
                       current->pid, zombie->pid, zombie->real_parent->pid);
            }
        }
        
        up(&shared_buffer->mutex);
        up(&shared_buffer->empty);
    }
    return 0;
}

static int __init zombie_killer_init(void)
{
    int i;

    // Validate parameters
    if (prod != 1 || cons < 1 || size < 1 || uid < 0) {
        printk(KERN_ERR "Invalid parameters\n");
        return -EINVAL;
    }

    // Allocate shared buffer
    shared_buffer = kmalloc(sizeof(struct buffer), GFP_KERNEL);
    if (!shared_buffer)
        return -ENOMEM;

    shared_buffer->items = kmalloc(sizeof(struct task_struct *) * size, GFP_KERNEL);
    if (!shared_buffer->items) {
        kfree(shared_buffer);
        return -ENOMEM;
    }

    // Initialize buffer
    shared_buffer->in = 0;
    shared_buffer->out = 0;
    shared_buffer->count = 0;
    sema_init(&shared_buffer->empty, size);
    sema_init(&shared_buffer->full, 0);
    sema_init(&shared_buffer->mutex, 1);

    // Create producer thread
    producer_thread = kmalloc(sizeof(struct task_struct *), GFP_KERNEL);
    if (!producer_thread) {
        kfree(shared_buffer->items);
        kfree(shared_buffer);
        return -ENOMEM;
    }

    producer_thread[0] = kthread_run(producer_fn, NULL, "producer");
    if (IS_ERR(producer_thread[0])) {
        kfree(producer_thread);
        kfree(shared_buffer->items);
        kfree(shared_buffer);
        return PTR_ERR(producer_thread[0]);
    }

    // Create consumer threads
    consumer_threads = kmalloc(sizeof(struct task_struct *) * cons, GFP_KERNEL);
    if (!consumer_threads) {
        kthread_stop(producer_thread[0]);
        kfree(producer_thread);
        kfree(shared_buffer->items);
        kfree(shared_buffer);
        return -ENOMEM;
    }

    for (i = 0; i < cons; i++) {
        consumer_threads[i] = kthread_run(consumer_fn, NULL, "consumer-%d", i);
        if (IS_ERR(consumer_threads[i])) {
            int j;
            for (j = 0; j < i; j++)
                kthread_stop(consumer_threads[j]);
            kthread_stop(producer_thread[0]);
            kfree(consumer_threads);
            kfree(producer_thread);
            kfree(shared_buffer->items);
            kfree(shared_buffer);
            return PTR_ERR(consumer_threads[i]);
        }
    }

    return 0;
}

static void __exit zombie_killer_exit(void)
{
    int i;

    // Stop all threads
    for (i = 0; i < cons; i++)
        kthread_stop(consumer_threads[i]);
    kthread_stop(producer_thread[0]);

    // Free allocated memory
    kfree(consumer_threads);
    kfree(producer_thread);
    kfree(shared_buffer->items);
    kfree(shared_buffer);

    printk(KERN_INFO "Zombie killer module unloaded\n");
}

module_init(zombie_killer_init);
module_exit(zombie_killer_exit);
