#include "timer.h"
#include "ng.h"
#include "hwtimer.h"
#include "hwatomic.h"
#include <assert.h>
#include "framework_defs.h"

#ifdef NODE_GLOBALS
    #warning Default Timer implementation used when NODE_GLOBALS is active. Are you sure this is what you want ??
#endif

#define HW_TIMER_ID 0

#define COUNTER_OVERFLOW_INCREASE (1<<(8*sizeof(timer_tick_t)))

static timer_event NGDEF(timer)[TIMER_EVENT_STACK_SIZE];
static volatile uint32_t NGDEF(next_event);
static volatile bool NGDEF(hw_event_scheduled);
static volatile uint32_t NGDEF(timer_offset);
enum
{
    NO_EVENT = TIMER_EVENT_STACK_SIZE,
};

static void timer_overflow();
static void timer_fired();

void timer_init()
{
    for(uint32_t i = 0; i < TIMER_EVENT_STACK_SIZE; i++)
	NG(timers)[i].f = 0x0;

    NG(next_event) = NO_EVENT;
    NG(timer_offset) = 0;
    NG(hw_event_scheduled) = false;
    //change this to ms once we know the overflows work
    error_t err hw_timer_init(HW_TIMER_ID, HWTIMER_FREQ_32K, &timer_fired, &timer_overflow);
    assert(err == SUCCESS);

}

error_t timer_post_event(task_t task, int32_t fire_time, uint8_t priority)
{
    error_t status = ENOMEM;
    if(fire_time < 0 || priority > MIN_PRIORITY)
	return EINVAL;

    start_atomic();

    for(uint32_t i = 0; i < TIMER_EVENT_STACK_TRACE; i++)
    {
	if(NG(timers)[i].f == 0x0)
	{
	    update_timers();
	    NG(timers)[i].f = task;
	    NG(timers)[i].next_event = fire_time;
	    NG(timers)[i].priority = priority;

	    //if there is no event scheduled, or this event is scheduled to be run
	    //before the first scheduled event: trigger a reschedule
	    if(NG(next_event) == NO_EVENT || NG(timers)[NG(next_event)].next_event > fire_time)
		timer_configure_next_event();
	    status = SUCCESS;
	    break;
	}
    }
    
    end_atomic();

    return status;
}

error_t timer_cancel_event(task_t task)
{
    error_t status = EALREADY;
    
    start_atomic();

    for(uint32_t i = 0; i < TIMER_EVENT_STACK_TRACE; i++)
    {
	if(NG(timers)[i].f == task)
	{
	    NG(timers)[i].f = 0x0;
	    //if we were the first event to fire --> trigger a reconfiguration
	    if(NG(next_event) == i)
		timer_configure_next_event();
	    status = SUCCESS;
	    break;
	}
    }
    end_atomic();

    return status;
}



static inline void reset_counter()
{
    //this function should only be called from an atomic context
    NG(timer_offset) = 0;
    hw_timer_counter_reset(HW_TIMER_ID);
}

uint32_t timer_get_counter_value()
{
    uint32_t counter;
    start_atomic();
	counter = NG(timer_offset) + hw_timer_getvalue(HW_TIMER_ID);
	//increase the counter with COUNTER_OVERFLOW_INCREASE
	//if an overflow is pending
	if(hw_timer_is_overflow_pending(HW_TIMER_ID))
	    counter += COUNTER_OVERFLOW_INCREASE;
    end_atomic();
    return counter;
}


static void update_timers()
{
    //this function should only be called from an atomic context

#ifdef FRAMEWORK_TIMER_RESET_COUNTER

    if(NG(next_event) == NO_EVENT)
    {
	reset_counter();
	return;
    }

    uint32_t cur_value = timer_get_counter_value();
    reset_counter();

    for(uint32_t i = 0; i < TIMER_EVENT_STACK_SIZE; i++)
    {
	if(NG(timers)[i].f != 0x0 && NG(timers)[i].next_event >= cur_value)
	    NG(timers)[i].next_event -= cur_value;	
    }
#endif //TIMER_RESET_COUNTER_ON_UPDATE

}

uint32_t get_next_event()
{
    //this function should only be called from an atomic context
    update_timers();
    uint32_t next_fire_time = 0xFFFFFFFF;
    uint32_t next_fire_event = NO_EVENT;
    for(uint32_t i = 0; i < TIMER_EVENT_STACK_SIZE; i++)
    {
	if(NG(timers)[i].f != 0x0 && NG(timers)[i].next_event < next_fire_time)
	{
	    next_fire_time = NG(timers)[i].next_event;
	    next_fire_event = i;
	}
    }
    return next_fire_event;
}


void configure_next_event()
{
    //this function should only be called from an atomic context
    uint32_t next_fire_time;
    do
    {
	//find the next event that has not yet passed, and schedule
	//the 'late' events while we're at it
	NG(next_event) = get_next_event();
	next_fire_time = NG(timers)[i].next_event;
	if(next_fire_time <= timer_get_counter_value())
	{
	    sched_post_task(NG(timers)[NG(next_event)].f, NG(timers)[NG(next_event)].priority);
	    NG(timers)[NG(next_event)].f = 0x0;
	}	    
    }
    while(NG(next_event) != NO_EVENT && next_fire_time <= timer_get_counter_value());

    //at this point NG(next_event) is eiter equal to NO_EVENT (no tasks left)
    //or we have the next event we can schedule
    if(NG(next_event) == NO_EVENT)
    {
	//cancel the timer in case it is still running (can happen if we're called from timer_cancel_event)
	NG(hw_event_scheduled) = false;
	hw_timer_cancel(HW_TIMER_ID);
    }
    else
    {
	//calculate schedule time relative to current time rather than
	//latest overflow time to 
	uint32_t fire_time = (timer_tick_t)(next_fire_time - timer_get_counter_value());
	if(fire_time < COUNTER_OVERFLOW_INCREASE)
	{
	    NG(hw_event_scheduled) = true
	    hw_timer_schedule(HW_TIMER_ID, (timer_tick_t)fire_time);
#ifndef NDEBUG	    
	    //check that we didn't try to schedule a timer in the past
	    //normally this shouldn't happen but it IS theoretically possible...
	    fire_time = (timer_tick_t)(next_fire_time - timer_get_counter_value());
	    assert(fire_time < 0xFFFF0000);
#endif
	}
    }
}

static void timer_overflow()
{
    NG(timer_offset) += COUNTER_OVERFLOW_INCREASE;
    if(NG(next_event) != NO_EVENT && 		//there is an event scheduled at THIS timer level
	(!NG(hw_event_scheduled)) &&		//but NOT at the hw timer level
	NG(timers)[NG(next_event)].next_event <= (NG(timer_offset) + COUNTER_OVERFLOW_INCREASE //and the next trigger will happen before the next overflow
	)
    {
	timer_tick_t fire_time = (timer_tick_t)(NG(timers)[NG(next_event)].next_event - NG(timer_offset));
	//fire time already passed
	if(fire_time > hw_timer_getvalue(HW_TIMER_ID))
	    timer_fired();
	else
	{
	    NG(hw_event_scheduled) = true;
	    hw_timer_schedule(HW_TIMER_ID, fire_time);
	}	
    }
}
static void timer_fired()
{
    assert(NG(next_event) != NO_EVENT)
    assert(NG(timers)[NG(next_event)].f != 0x0);
    sched_post_task(NG(timers)[NG(next_event)].f, NG(timers)[NG(next_event)].priority);
    NG(timers)[NG(next_event)].f = 0x0;
    configure_next_event();
}
