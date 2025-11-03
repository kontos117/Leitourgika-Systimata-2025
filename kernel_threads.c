
#include "tinyos.h"
#include "kernel_sched.h"
#include "kernel_proc.h"
#include "kernel_cc.h"
#include "kernel_streams.h"

/** 
  @brief Create a new thread in the current process.
  */
Tid_t sys_CreateThread(Task task, int argl, void* args)
{
  PTCB* ptcb = acquire_PTCB();
  initialise_PTCB(ptcb, task, argl, args);

  CURPROC -> thread_count++;

  if(task != NULL) {
    TCB* tcb = spawn_thread(CURPROC, start_other_thread);
    ptcb->tcb = tcb;
    tcb->ptcb = ptcb;

    rlist_push_back(&CURPROC->ptcb_list,&ptcb->ptcb_list_node);

    wakeup(ptcb->tcb);
  }

	return (Tid_t) ptcb;
}

/**
  @brief Return the Tid of the current thread.
 */
Tid_t sys_ThreadSelf()
{
	return (Tid_t) cur_thread() -> ptcb;
}

/**
  @brief Join the given thread.
  */
int sys_ThreadJoin(Tid_t tid, int* exitval)
{
  
  rlnode* list = &CURPROC -> ptcb_list;
  rlnode* node = rlist_find(list, (PTCB*) tid, NULL);
  // return -1 if there is no thread with the given tid in this process
  if(node == NULL) return -1;

  PTCB* ptcb = node->ptcb;
  /* return -1 if the tid corresponds to the current thread or 
    the tid corresponds to a detached thread */

  if(tid == sys_ThreadSelf() || ptcb->detached ) return -1;

  ptcb->refcount++;
  while(!ptcb->exited && !ptcb->detached)
  {
    kernel_wait(&ptcb->exit_cv,SCHED_USER);
  }
  ptcb->refcount--;

  // last check before passing exitval
  if(ptcb->detached) return -1;

  // if exitval has a value and then return value
  if(exitval) *exitval = ptcb->exitval;

  if(ptcb->refcount == 0) {
    //fprintf(stderr, "threadJoin free\n");
    release_PTCB(ptcb);
  }

	return 0;
}

/**
  @brief Detach the given thread.
  */
int sys_ThreadDetach(Tid_t tid)
{
  PTCB* ptcb = (PTCB*) tid;

  rlnode* list = &CURPROC->ptcb_list;
  rlnode* node = rlist_find(list, ptcb, NULL);
  /* return -1 if there is no thread with the given tid in this process
    or the tid corresponds to an exited thread */
  if(node==NULL || ptcb->exited) return -1;

  if(!ptcb->detached) {
    ptcb->detached = 1;
    kernel_broadcast(&ptcb->exit_cv);
  }
  
  return 0;
}

/**
  @brief Terminate the current thread.
  */
void sys_ThreadExit(int exitval)
{
  PTCB* ptcb = cur_thread()->ptcb;
  ptcb->exitval = exitval;
  ptcb->exited = 1;

  kernel_broadcast(&ptcb->exit_cv);
  CURPROC->thread_count--;

  //if(ptcb->detached || ptcb->refcount == 0) {
  if(ptcb->detached) {
    //fprintf(stderr, "threadExit free\n");
    release_PTCB(ptcb);
  }

  if(CURPROC->thread_count == 0) {

    PCB *curproc = CURPROC;  /* cache for efficiency */

    if(get_pid(curproc) != 1) { // sys_Exit had this block of code 
                                // in else{} (if statement was get_pid(curproc)==1)
        /* Reparent any children of the exiting process to the 
        initial task */
      PCB* initpcb = get_pcb(1);
      while(!is_rlist_empty(& curproc->children_list)) {
        rlnode* child = rlist_pop_front(& curproc->children_list);
        child->pcb->parent = initpcb;
        rlist_push_front(& initpcb->children_list, child);
      }

      /* Add exited children to the initial task's exited list 
        and signal the initial task */
      if(!is_rlist_empty(& curproc->exited_list)) {
        rlist_append(& initpcb->exited_list, &curproc->exited_list);
        kernel_broadcast(& initpcb->child_exit);
      }

      /* Put me into my parent's exited list */
      rlist_push_front(& curproc->parent->exited_list, &curproc->exited_node);
      kernel_broadcast(& curproc->parent->child_exit);


      assert(is_rlist_empty(& curproc->children_list));
      assert(is_rlist_empty(& curproc->exited_list));
    }

    /* 
      Do all the other cleanup we want here, close files etc. 
    */

    /* Release the args data */
    if(curproc->args) {
      free(curproc->args);
      curproc->args = NULL;
    }

    /* Clean up FIDT */
    for(int i=0;i<MAX_FILEID;i++) {
      if(curproc->FIDT[i] != NULL) {
        FCB_decref(curproc->FIDT[i]);
        curproc->FIDT[i] = NULL;
      }
    }

    /* Disconnect my main_thread */
    curproc->main_thread = NULL;

    /* Now, mark the process as exited. */
    curproc->pstate = ZOMBIE;

  }

  /* Bye-bye cruel world */
  kernel_sleep(EXITED, SCHED_USER);
}

