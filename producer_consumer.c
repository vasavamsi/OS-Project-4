#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/semaphore.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>

static int prod = 1; // Always set to 1
static int cons = 0; // Set based on user input
static int size = 10; // Default size of the circular buffer
static int uid = 0;  // UID of the test user
static int exit_flag = 0; // Flag to signal exit

module_param(prod, int, 0);
module_param(cons, int, 0);
module_param(size, int, 0);
module_param(uid, int, 0);

static struct semaphore empty;
static struct semaphore full;
static struct task_struct **zombie_buffer; // Pointer for the circular buffer
static struct task_struct **consumer_threads; // Array to store consumer thread pointers
static int total_consumers = 0; // Track number of consumer threads
static int in = 0; // Index for producer
static int out = 0; // Index for consumer

static struct task_struct *producer_thread; // Pointer for the producer thread

#define EXIT_ZOMBIE 0x00000020 // Define exit state for zombies

int producer_function(void *data) {
    char thread_name[16];
    snprintf(thread_name, sizeof(thread_name), "Producer-%d", 1);

    while (!kthread_should_stop()) {
        if (exit_flag) // Check if we should exit
            break;

        struct task_struct *p;
        for_each_process(p) {
            if (p->cred->uid.val == uid && (p->exit_state & EXIT_ZOMBIE)) {
                down_interruptible(&empty);
                zombie_buffer[in] = p;
                printk(KERN_INFO "[%s] has produced a zombie process with pid %d and parent pid %d\n",
                       thread_name, p->pid, p->real_parent->pid);
                in = (in + 1) % size;
                up(&full);
            }
        }
        msleep(100);
    }
    return 0;
}

int consumer_function(void *data) {
    int id = *(int *)data;
    char thread_name[16];
    snprintf(thread_name, sizeof(thread_name), "Consumer-%d", id);
    kfree(data);

    while (!kthread_should_stop() && !exit_flag) {
        if (down_interruptible(&full))
            break;

        if (kthread_should_stop() || exit_flag) {
            up(&full);
            break;
        }

        struct task_struct *zombie = zombie_buffer[out];
        if (zombie) {
            printk(KERN_INFO "[%s] has consumed a zombie process with pid %d and parent pid %d\n",
                   thread_name, zombie->pid, zombie->real_parent->pid);
            kill_pid(zombie->real_parent->thread_pid, SIGKILL, 0);
            out = (out + 1) % size;
        }
        up(&empty);
        msleep(100);
    }
    return 0;
}

static int __init zombie_killer_init(void) {
    int i;

    if (size <= 0) {
        printk(KERN_ERR "Invalid buffer size: %d. Must be greater than 0.\n", size);
        return -EINVAL;
    }

    consumer_threads = kmalloc_array(cons, sizeof(struct task_struct *), GFP_KERNEL);
    if (!consumer_threads) {
        printk(KERN_ERR "Failed to allocate memory for consumer threads array.\n");
        return -ENOMEM;
    }

    zombie_buffer = kmalloc_array(size, sizeof(struct task_struct *), GFP_KERNEL);
    if (!zombie_buffer) {
        printk(KERN_ERR "Failed to allocate memory for zombie buffer.\n");
        kfree(consumer_threads);
        return -ENOMEM;
    }

    for (i = 0; i < size; i++) {
        zombie_buffer[i] = NULL;
    }

    sema_init(&empty, size);
    sema_init(&full, 0);

    producer_thread = kthread_run(producer_function, NULL, "Producer-1");
    if (!producer_thread) {
        printk(KERN_ERR "Failed to create producer thread.\n");
        kfree(zombie_buffer);
        kfree(consumer_threads);
        return -1;
    }
    printk(KERN_INFO "Producer thread started.\n");

    for (i = 0; i < cons; i++) {
        int *id = kmalloc(sizeof(int), GFP_KERNEL);
        *id = i + 1;
        consumer_threads[i] = kthread_run(consumer_function, id, "Consumer-%d", *id);
        if (!consumer_threads[i]) {
            printk(KERN_ERR "Failed to create consumer thread %d.\n", *id);
            kfree(id);
            while (--i >= 0) {
                kthread_stop(consumer_threads[i]);
            }
            kthread_stop(producer_thread);
            kfree(consumer_threads);
            kfree(zombie_buffer);
            return -1;
        }
        total_consumers++;
        printk(KERN_INFO "Consumer thread %d started.\n", *id);
    }

    return 0;
}

static void __exit zombie_killer_exit(void) {
    int i;
    exit_flag = 1;
    msleep(200); // Wait for threads to see exit flag

    if (producer_thread) {
        kthread_stop(producer_thread);
        msleep(100);
    }

    // Wake up any waiting consumers
    for (i = 0; i < cons; i++) {
        up(&full);
        up(&empty);
    }

    msleep(100);

    for (i = 0; i < total_consumers; i++) {
        if (consumer_threads[i]) {
            kthread_stop(consumer_threads[i]);
            msleep(50);
        }
    }

    msleep(100);
    kfree(consumer_threads);
    kfree(zombie_buffer);
    printk(KERN_INFO "Zombie Killer module unloaded.\n");
}

module_init(zombie_killer_init);
module_exit(zombie_killer_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Vamsi Krishna Vasa");
MODULE_DESCRIPTION("Zombie Killer: A module to identify and kill zombie processes.");
