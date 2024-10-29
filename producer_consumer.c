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
            // Check for zombie process and UID match
            if (p->cred->uid.val == uid && (p->exit_state & EXIT_ZOMBIE)) {
                // Store the zombie in the buffer
                down_interruptible(&empty);
                zombie_buffer[in] = p;
                printk(KERN_INFO "[%s] has produced a zombie process with pid %d and parent pid %d\n",
                       thread_name, p->pid, p->real_parent->pid);
                in = (in + 1) % size; // Circular increment
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
            out = (out + 1) % size; // Circular increment
        }
        up(&empty);
    }
    return 0;
}

static int __init zombie_killer_init(void) {
    int i;

    if (size <= 0) {
        printk(KERN_ERR "Invalid buffer size: %d. Must be greater than 0.\n", size);
        return -EINVAL; // Return an error if size is invalid
    }

    zombie_buffer = kmalloc_array(size, sizeof(struct task_struct *), GFP_KERNEL);
    if (!zombie_buffer) {
        printk(KERN_ERR "Failed to allocate memory for zombie buffer.\n");
        return -ENOMEM; // Return an error if memory allocation fails
    }

    sema_init(&empty, size);
    sema_init(&full, 0);

    // Start producer thread (only one)
    producer_thread = kthread_run(producer_function, NULL, "Producer-1");
    if (!producer_thread) {
        printk(KERN_ERR "Failed to create producer thread.\n");
        kfree(zombie_buffer); // Free the buffer memory if thread creation fails
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
            kfree(zombie_buffer); // Free the buffer memory
            return -1; // Return an error
        }
        printk(KERN_INFO "Consumer thread %d started.\n", *id);
    }

    return 0;
}

static void __exit zombie_killer_exit(void) {
    exit_flag = 1; // Signal the producer thread to exit

    // Stop the producer thread
    if (producer_thread) {
        kthread_stop(producer_thread);
        printk(KERN_INFO "Producer thread stopped.\n");
    }

    kfree(zombie_buffer); // Free the allocated buffer
    printk(KERN_INFO "Zombie Killer module unloaded.\n");
}

module_init(zombie_killer_init);
module_exit(zombie_killer_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Vamsi Krishna Vasa"); // Replace with your name
MODULE_DESCRIPTION("Zombie Killer: A module to identify and kill zombie processes.");
