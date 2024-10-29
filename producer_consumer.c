#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/semaphore.h>
#include <linux/slab.h>

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

// Bounded buffer structure
struct task_struct **buffer;
int in = 0;
int out = 0;
int count = 0;

// Synchronization primitives
struct semaphore empty;
struct semaphore full;
struct semaphore mutex;

// Thread handles
static struct task_struct *producer_thread;
static struct task_struct **consumer_threads;

// Producer function
static int producer(void *data)
{
    struct task_struct *task;
    
    while (!kthread_should_stop()) {
        for_each_process(task) {
            if (task->cred->uid.val == uid && 
                (task->exit_state & EXIT_ZOMBIE)) {
                
                down(&empty);
                down(&mutex);
                
                // Add zombie to buffer
                buffer[in] = task;
                in = (in + 1) % size;
                count++;
                
                printk(KERN_INFO "[Producer-%d] has produced a zombie process with pid %d and parent pid %d\n",
                       current->pid, task->pid, task->parent->pid);
                
                up(&mutex);
                up(&full);
            }
        }
        msleep(250);
    }
    return 0;
}

// Consumer function
static int consumer(void *data)
{
    struct task_struct *zombie;
    int consumer_id = *(int*)data;
    
    while (!kthread_should_stop()) {
        down(&full);
        down(&mutex);
        
        // Get zombie from buffer
        zombie = buffer[out];
        out = (out + 1) % size;
        count--;
        
        printk(KERN_INFO "[Consumer-%d] has consumed a zombie process with pid %d and parent pid %d\n",
               current->pid, zombie->pid, zombie->parent->pid);
        
        // Kill zombie's parent
        kill_pid(find_vpid(zombie->parent->pid), SIGKILL, 1);
        
        up(&mutex);
        up(&empty);
    }
    return 0;
}

static int __init zombie_killer_init(void)
{
    int i;
    int *consumer_ids;
    
    // Validate parameters
    if (prod != 1 || cons < 1 || size < 1) {
        printk(KERN_ERR "Invalid parameters\n");
        return -EINVAL;
    }
    
    // Allocate buffer
    buffer = kmalloc(sizeof(struct task_struct*) * size, GFP_KERNEL);
    if (!buffer) {
        return -ENOMEM;
    }
    
    // Initialize semaphores
    sema_init(&empty, size);
    sema_init(&full, 0);
    sema_init(&mutex, 1);
    
    // Create producer thread
    producer_thread = kthread_run(producer, NULL, "producer");
    if (IS_ERR(producer_thread)) {
        kfree(buffer);
        return PTR_ERR(producer_thread);
    }
    
    // Allocate consumer threads array
    consumer_threads = kmalloc(sizeof(struct task_struct*) * cons, GFP_KERNEL);
    consumer_ids = kmalloc(sizeof(int) * cons, GFP_KERNEL);
    if (!consumer_threads || !consumer_ids) {
        kfree(buffer);
        kfree(consumer_threads);
        kfree(consumer_ids);
        kthread_stop(producer_thread);
        return -ENOMEM;
    }
    
    // Create consumer threads
    for (i = 0; i < cons; i++) {
        consumer_ids[i] = i + 1;
        consumer_threads[i] = kthread_run(consumer, &consumer_ids[i], "consumer-%d", i + 1);
        if (IS_ERR(consumer_threads[i])) {
            int j;
            for (j = 0; j < i; j++) {
                kthread_stop(consumer_threads[j]);
            }
            kthread_stop(producer_thread);
            kfree(buffer);
            kfree(consumer_threads);
            kfree(consumer_ids);
            return PTR_ERR(consumer_threads[i]);
        }
    }
    
    printk(KERN_INFO "Zombie killer module loaded\n");
    return 0;
}

static void __exit zombie_killer_exit(void)
{
    int i;
    
    // Stop all threads
    kthread_stop(producer_thread);
    for (i = 0; i < cons; i++) {
        kthread_stop(consumer_threads[i]);
    }
    
    // Free allocated memory
    kfree(buffer);
    kfree(consumer_threads);
    
    printk(KERN_INFO "Zombie killer module unloaded\n");
}

module_init(zombie_killer_init);
module_exit(zombie_killer_exit);
