#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/semaphore.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>

#define BUFFER_SIZE 10 // Define a max size for the circular buffer
#define EXIT_ZOMBIE 0x00000020 // Define exit state for zombies

static int prod = 1; // Always set to 1
static int cons = 0; // Set based on user input
static int size = 0; // Size of the circular buffer
static int uid = 0;  // UID of the test user

module_param(prod, int, 0);
module_param(cons, int, 0);
module_param(size, int, 0);
module_param(uid, int, 0);

static struct semaphore empty;
static struct semaphore full;
static struct task_struct *zombie_buffer[BUFFER_SIZE]; // Circular buffer for zombies
static int in = 0; // Index for producer
static int out = 0; // Index for consumer

int producer_function(void *data) {
    char thread_name[16];
    snprintf(thread_name, sizeof(thread_name), "Producer-%d", 1);

    while (!kthread_should_stop()) {
        struct task_struct *p;
        for_each_process(p) {
            // Check for zombie process and UID match
            if (p->cred->uid.val == uid && (p->exit_state & EXIT_ZOMBIE)) {
                // Store the zombie in the buffer
                down_interruptible(&empty);
                zombie_buffer[in] = p;
                printk(KERN_INFO "[%s] has produced a zombie process with pid %d and parent pid %d\n",
                       thread_name, p->pid, p->real_parent->pid);
                in = (in + 1) % BUFFER_SIZE; // Circular increment
                up(&full);
            }
        }
        msleep(250); // Sleep to reduce CPU usage
    }
    return 0;
}

int consumer_function(void *data) {
    int id = *(int *)data;
    char thread_name[16];
    snprintf(thread_name, sizeof(thread_name), "Consumer-%d", id);

    while (!kthread_should_stop()) {
        down_interruptible(&full);
        // Consume the zombie from the buffer
        struct task_struct *zombie = zombie_buffer[out];
        if (zombie) {
            // Kill the parent of the zombie
            printk(KERN_INFO "[%s] has consumed a zombie process with pid %d and parent pid %d\n",
                   thread_name, zombie->pid, zombie->real_parent->pid);
            kill_pid(zombie->real_parent->thread_pid, SIGKILL, 0);
            out = (out + 1) % BUFFER_SIZE; // Circular increment
        }
        up(&empty);
    }
    return 0;
}

static int __init zombie_killer_init(void) {
    int i;

    sema_init(&empty, BUFFER_SIZE);
    sema_init(&full, 0);

    // Start producer thread (only one)
    struct task_struct *producer_thread = kthread_run(producer_function, NULL, "Producer-1");
    if (!producer_thread) {
        printk(KERN_ERR "Failed to create producer thread.\n");
        return -1; // Return an error if thread creation fails
    }
    printk(KERN_INFO "Producer thread started.\n");

    // Start consumer threads
    for (i = 0; i < cons; i++) {
        int *id = kmalloc(sizeof(int), GFP_KERNEL);
        *id = i + 1;
        struct task_struct *consumer_thread = kthread_run(consumer_function, id, "Consumer-%d", *id);
        if (!consumer_thread) {
            printk(KERN_ERR "Failed to create consumer thread %d.\n", *id);
            kfree(id); // Free the memory if thread creation fails
            return -1; // Return an error
        }
        printk(KERN_INFO "Consumer thread %d started.\n", *id);
    }

    return 0;
}

static void __exit zombie_killer_exit(void) {
    printk(KERN_INFO "Zombie Killer module unloaded.\n");
}

module_init(zombie_killer_init);
module_exit(zombie_killer_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Vamsi Krishna Vasa"); // Replace with your name
MODULE_DESCRIPTION("Zombie Killer: A module to identify and kill zombie processes.");
