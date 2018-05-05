/*
 * elevator look
 */
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>

struct look_data {
	struct list_head queue;
	int moving_up;
	int allow_front_merges;
	unsigned long long disk_head;
};

void print_elevator_queue_order(struct request_queue *q) 
{
	struct look_data *ld = q->elevator->elevator_data;
	int counter = 0;
	int ascending = 0;
	int descending = 0;

	printk(KERN_DEBUG "***LOOK_SCHED: Queue = ");
	if (!list_empty(&ld->queue)) {
		struct request *rq_iter = list_entry(ld->queue.next, struct request, queuelist);			
		struct request *rq_iter_last = NULL;

		while(1) {
			counter++;
			printk(KERN_CONT "|%llu", blk_rq_pos(rq_iter));
			if(list_is_last(&rq_iter->queuelist, &ld->queue)) {
				break;
			}
			rq_iter_last = rq_iter;
			rq_iter = list_entry(rq_iter->queuelist.next, struct request, queuelist);
			if(blk_rq_pos(rq_iter_last) < blk_rq_pos(rq_iter)) {
				ascending = 1;
			} else if(blk_rq_pos(rq_iter_last) > blk_rq_pos(rq_iter)) {
				descending = 1;
			}	
		}

		if(ascending) {
			printk(KERN_CONT " LOOK_ASCENDING");
		}

		if(descending) {
			printk(KERN_CONT " LOOK_DESCENDING");
		}

		if(counter > 3) {
			printk(KERN_CONT " LOOK_LONG");
		}
	} else {
		printk(KERN_CONT "EMPTY");
	}
	printk(KERN_CONT "\n");
}

static void look_merged_request(struct request_queue *q, struct request *req, int type)
{
	//Don't need to do anything because merges don't affect request order for LOOK scheduler.
}

static void look_merged_requests(struct request_queue *q, struct request *rq,
				 struct request *next)
{
	//Don't need to do anything because merges don't affect request order for LOOK scheduler.
	list_del_init(&next->queuelist);
}

static struct request * look_find_fmerge(struct look_data *ld, sector_t sector) {
	struct request *rq;

	if(!list_empty(&ld->queue)) {
		rq = list_entry(ld->queue.next, struct request, queuelist);
		while(1) {
			if(blk_rq_pos(rq) == sector) {
				return rq;
			}

			if(list_is_last(&rq->queuelist, &ld->queue)) {
				break;
			}
			rq = list_entry(rq->queuelist.next, struct request, queuelist);
		}		
	}

	return NULL;
}

static int look_merge(struct request_queue *q, struct request **req, struct bio *bio)
{
	struct look_data *ld = q->elevator->elevator_data;
	struct request *rq;

	if (ld->allow_front_merges) {
		sector_t sector = bio_end_sector(bio);

		rq = look_find_fmerge(ld, sector);
		if (rq) {
			printk(KERN_DEBUG "***LOOK_SCHED: Found front merge at position %llu.\n", blk_rq_pos(rq));
			BUG_ON(sector != blk_rq_pos(rq));

			if (elv_rq_merge_ok(rq, bio)) {
				*req = rq;
				return ELEVATOR_FRONT_MERGE;
			}
		}
	}

	return ELEVATOR_NO_MERGE;
}

static int look_dispatch(struct request_queue *q, int force)
{
	struct look_data *ld = q->elevator->elevator_data;

	if (!list_empty(&ld->queue)) {
		struct request *rq, *rq_new_next = NULL;
		rq = list_entry(ld->queue.next, struct request, queuelist);
		if(!list_is_last(&rq->queuelist, &ld->queue)) {
			rq_new_next = list_entry(rq->queuelist.next, struct request, queuelist);
			if(ld->moving_up != (ld->disk_head <= blk_rq_pos(rq_new_next))) {
				ld->moving_up = !ld->moving_up;
				if(ld->moving_up) {
					printk(KERN_DEBUG "***LOOK_SCHED: Scan directon changed to UP.\n");
				} else {
					printk(KERN_DEBUG "***LOOK_SCHED: Scan directon changed to DOWN.\n");
				}
			}
		}
		ld->disk_head = blk_rq_pos(rq);
		list_del_init(&rq->queuelist);
		elv_dispatch_add_tail(q, rq);
		print_elevator_queue_order(q);
		return 1;
	} 	
	return 0;
}

static void look_add_request(struct request_queue *q, struct request *rq)
{
	struct look_data *ld = q->elevator->elevator_data;

	if (list_empty(&ld->queue)) {
		list_add_tail(&rq->queuelist, &ld->queue);
		ld->moving_up = (ld->disk_head <= blk_rq_pos(rq));	
		printk(KERN_DEBUG "***LOOK_SCHED: Adding request for position (%llu) to empty queue.\n", blk_rq_pos(rq));
		print_elevator_queue_order(q);
	} else {
		struct request *rq_iter = list_entry(ld->queue.next, struct request, queuelist);			
		struct request *rq_iter_next = list_entry(rq_iter->queuelist.next, struct request, queuelist);
		if(ld->moving_up != (blk_rq_pos(rq_iter) <= blk_rq_pos(rq))) {
			//Move rq_iter to before first request of the sweep of the disk head that new element will be accessed in (either up sweep or down sweep)
			while(ld->moving_up == (blk_rq_pos(rq_iter) <= blk_rq_pos(rq_iter_next))) {
					rq_iter = rq_iter_next;
					if(!list_is_last(&rq_iter->queuelist, &ld->queue)) {
						rq_iter_next = list_entry(rq_iter->queuelist.next, struct request, queuelist);
					} else {
						break;
					}
			}
		}

		//Move to rq_iter to element after which rq should be placed
		while(!list_is_last(&rq_iter->queuelist, &ld->queue) && (blk_rq_pos(rq_iter) <= blk_rq_pos(rq_iter_next)) == (blk_rq_pos(rq_iter_next) <= blk_rq_pos(rq))) {
			rq_iter = rq_iter_next;
			if(!list_is_last(&rq_iter->queuelist, &ld->queue)) {
				rq_iter_next = list_entry(rq_iter->queuelist.next, struct request, queuelist);
			}
		}

		list_add(&rq->queuelist, &rq_iter->queuelist);	
		printk(KERN_DEBUG "***LOOK_SCHED: Adding request for position (%llu) to queue.\n", blk_rq_pos(rq));
		print_elevator_queue_order(q);
	}

}

static struct request *
look_former_request(struct request_queue *q, struct request *rq)
{
	struct look_data *ld = q->elevator->elevator_data;

	if (rq->queuelist.prev == &ld->queue)
		return NULL;
	return list_entry(rq->queuelist.prev, struct request, queuelist);
}

static struct request *
look_latter_request(struct request_queue *q, struct request *rq)
{
	struct look_data *ld = q->elevator->elevator_data;

	if (rq->queuelist.next == &ld->queue)
		return NULL;
	return list_entry(rq->queuelist.next, struct request, queuelist);
}

static int look_init_queue(struct request_queue *q, struct elevator_type *e)
{
	struct look_data *ld;
	struct elevator_queue *eq;

	eq = elevator_alloc(q, e);
	if (!eq)
		return -ENOMEM;

	ld = kmalloc_node(sizeof(*ld), GFP_KERNEL, q->node);
	if (!ld) {
		kobject_put(&eq->kobj);
		return -ENOMEM;
	}
	eq->elevator_data = ld;

	INIT_LIST_HEAD(&ld->queue);
	ld->disk_head = 0;
	ld->allow_front_merges = 1;

	spin_lock_irq(q->queue_lock);
	q->elevator = eq;
	spin_unlock_irq(q->queue_lock);
	return 0;
}

static void look_exit_queue(struct elevator_queue *e)
{
	struct look_data *ld = e->elevator_data;

	BUG_ON(!list_empty(&ld->queue));
	kfree(ld);
}

static struct elevator_type elevator_look = {
	.ops = {
		.elevator_merge_fn			= look_merge,
		.elevator_merged_fn			= look_merged_request,
		.elevator_merge_req_fn		= look_merged_requests,
		.elevator_dispatch_fn		= look_dispatch,
		.elevator_add_req_fn		= look_add_request,
		.elevator_former_req_fn		= look_former_request,
		.elevator_latter_req_fn		= look_latter_request,
		.elevator_init_fn		= look_init_queue,
		.elevator_exit_fn		= look_exit_queue,
	},
	.elevator_name = "look",
	.elevator_owner = THIS_MODULE,
};

static int __init look_init(void)
{
	return elv_register(&elevator_look);
}

static void __exit look_exit(void)
{
	elv_unregister(&elevator_look);
}

module_init(look_init);
module_exit(look_exit);


MODULE_AUTHOR("Austin Row, Benjamin Richards, Lazar Sharipoff");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("LOOK IO scheduler");
