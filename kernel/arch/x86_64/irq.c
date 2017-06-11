/*
* Copyright (c) 2016, 2017 Pedro Falcato
* This file is part of Onyx, and is released under the terms of the MIT License
* check LICENSE at the root directory for more information
*/
/**************************************************************************
 *
 *
 * File: irq.c
 *
 * Description: Contains irq installation functions
 *
 * Date: 1/2/2016
 *
 *
 **************************************************************************/
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>

#include <kernel/scheduler.h>
#include <kernel/task_switching.h>
#include <kernel/panic.h>
#include <kernel/registers.h>
#include <kernel/irq.h>
#include <kernel/platform.h>
#include <kernel/cpu.h>
#include <kernel/idt.h>
#include <kernel/apic.h>

volatile _Bool is_in_irq = false;
#define NR_IRQ 221
#define NUM_IOAPIC_PINS	24
irq_list_t *irq_routines[NR_IRQ]  =
{
	NULL
};

_Bool isirq()
{
	return is_in_irq;
}

void irq_install_handler(int irq, irq_t handler)
{
	assert(irq < NR_IRQ);
	if(irq < NUM_IOAPIC_PINS)
		ioapic_unmask_pin((uint32_t) irq);
	irq_list_t *lst = irq_routines[irq];
	if(!lst)
	{
		lst = (irq_list_t*) malloc(sizeof(irq_list_t));
		if(!lst)
		{
			errno = ENOMEM;
			return; /* TODO: Return a value indicating an error */
		}
		memset(lst, 0, sizeof(irq_list_t));
		lst->handler = handler;
		irq_routines[irq] = lst;
		return;
	}
	while(lst->next != NULL)
		lst = lst->next;
	lst->next = (irq_list_t*) malloc(sizeof(irq_list_t));
	if(!lst->next)
	{
		errno = ENOMEM;
		return; /* See the above TODO */
	}
	lst->next->handler = handler;
	lst->next->next = NULL;
}

void irq_uninstall_handler(int irq, irq_t handler)
{
	irq_list_t *list = irq_routines[irq];
	if(list->handler == handler)
	{
		free(list);
		irq_routines[irq] = NULL;
		return;
	}
	irq_list_t *prev = NULL;
	while(list->handler != handler)
	{
		prev = list;
		list = list->next;
	}
	prev->next = list->next;
	free(list);
}

uintptr_t irq_handler(uint64_t irqn, registers_t *regs)
{
	if(irqn > NR_IRQ)
	{
		return (uintptr_t) regs;
	}
	uintptr_t ret = (uintptr_t) regs;
	irq_list_t *handlers = irq_routines[irqn];
	if(!handlers)
		printk("irq: Unhandled interrupt at IRQ %u\n", irqn);
	is_in_irq = true;
	for(irq_list_t *i = handlers; i != NULL;i = i->next)
	{
		irq_t handler = i->handler;
		uintptr_t p = handler(regs);
		if(p != 0)
		{
			ret = p;
		}
	}
	is_in_irq = false;
	return ret;
}

static struct irq_work *queue = NULL;
static thread_t *irq_worker_thread = NULL;
int irq_schedule_work(void (*callback)(void *, size_t), size_t payload_size, void *payload)
{
	thread_wake_up(irq_worker_thread);
	struct irq_work *q = queue;
	while(q->callback)
	{
		q = (struct irq_work *) (char*) &q->payload + q->payload_size;
	}
	uintptr_t remaining_size = ((uintptr_t) queue + IRQ_WORK_QUEUE_SIZE) - (uintptr_t) q;
	if(sizeof(struct irq_work) + payload_size > remaining_size)
		return 1;
	q->callback = callback;
	q->payload_size = payload_size;
	memcpy(&q->payload, payload, payload_size);
	return 0;
}

int irq_get_work(struct irq_work *strct)
{
	if(!queue->callback)
		return -1;
	memcpy(strct, queue, sizeof(struct irq_work) + queue->payload_size);
	struct irq_work *next_work = (struct irq_work*) (char*) queue + sizeof(struct irq_work) + strct->payload_size;
	memcpy(queue, next_work, IRQ_WORK_QUEUE_SIZE - sizeof(struct irq_work) - strct->payload_size);
	memset((char*) queue + sizeof(struct irq_work) + strct->payload_size, 0, 
	IRQ_WORK_QUEUE_SIZE - sizeof(struct irq_work) - strct->payload_size);
	return 0;
}

struct irq_work *worker_buffer = NULL;
void irq_worker(void *ctx)
{
	while(1)
	{
		/* Do any work needed */
		if(irq_get_work(worker_buffer) < 0)
		{
			thread_set_state(irq_worker_thread, THREAD_BLOCKED);
			sched_yield();
			continue;
		}
		worker_buffer->callback(&worker_buffer->payload, worker_buffer->payload_size);
	}
}

void irq_init(void)
{
	if(!(irq_worker_thread = sched_create_thread(irq_worker, 1, NULL)))
		panic("irq_init: Could not create the worker thread!\n");
	queue = malloc(IRQ_WORK_QUEUE_SIZE);
	if(!queue)
		panic("irq_init: failed to allocate queue!\n");
	memset(queue, 0, IRQ_WORK_QUEUE_SIZE);
	worker_buffer = malloc(IRQ_WORK_QUEUE_SIZE);
	if(!worker_buffer)
		panic("irq_init: failed to allocate buffer!\n");
	memset(worker_buffer, 0, IRQ_WORK_QUEUE_SIZE);
}

#define PCI_MSI_BASE_ADDRESS 0xFEE00000
#define PCI_MSI_APIC_ID_SHIFT	12
#define PCI_MSI_REDIRECTION_HINT	(1 << 3)
int platform_allocate_msi_interrupts(unsigned int num_vectors, bool addr64,
                                     struct pci_msi_data *data)
{
	/* TODO: Balance IRQs between processors, since it's not ok to assume
	 * the current CPU, since then, IRQs become unbalanced
	*/
	/* 
	 * TODO: Magenta hardcodes some of this stuff. Is it dangerous that things
	 * are hardcoded like that?
	*/
	struct processor *proc = get_processor_data();
	assert(proc != NULL);
	int vecs = x86_allocate_vectors(num_vectors);
	if(vecs < 0)
		return -1;
	/* See section 10.11.1 of the intel software developer manuals */
	uint32_t address = PCI_MSI_BASE_ADDRESS;
	address |= ((uint32_t) proc->lapic_id) << PCI_MSI_APIC_ID_SHIFT;
	
	/* See section 10.11.2 of the intel software developer manuals */
	uint32_t data_val = vecs;

	data->address = address;
	data->address_high = 0;
	data->data = data_val;
	data->vector_start = vecs;
	return 0;
}
void platform_send_eoi(uint64_t irq)
{
	/* Note: MSI interrupts also require EOIs */
	lapic_send_eoi();
}
