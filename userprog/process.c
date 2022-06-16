#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#include "userprog/process.h"
#ifdef VM // 살려!
#include "vm/vm.h"
#endif

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);

/* 후보 1 : argument passing 함수를 여기로 빼주기 */

/* General process initializer for initd and other process. */
static void
process_init (void) {
	struct thread *current = thread_current ();
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
tid_t
process_create_initd (const char *file_name) {
	char *fn_copy;
	tid_t tid;

	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy (fn_copy, file_name, PGSIZE);

	/* Create a new thread to execute FILE_NAME. */
	// 변경사항
	char *token, *save_ptr;
	token = strtok_r(file_name, " ", &save_ptr);
	// 변경사항
	tid = thread_create (file_name, PRI_DEFAULT, initd, fn_copy);
	if (tid == TID_ERROR)
		palloc_free_page (fn_copy);
	return tid;
}

/* A thread function that launches first user process. */
static void
initd (void *f_name) {
#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif

	process_init ();

	if (process_exec (f_name) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED ();
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
tid_t
process_fork (const char *name, struct intr_frame *if_ UNUSED) {
	/* Clone current thread to new thread.*/
	struct thread *curr = thread_current();
	// memcpy(&curr->parent_if, if_, sizeof(struct intr_frame)); 

	tid_t pid = thread_create (name, PRI_DEFAULT, __do_fork, curr); // 마지막에 thread_current를 줘서, 같은 rsi를 공유하게 함.
	if (pid == TID_ERROR)
		return TID_ERROR;
	
	struct thread *child = get_child(pid);
	sema_down(&child->fork_sema); 

	if(child->exit_status == -1)
		return TID_ERROR;
	return pid;
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux;
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. TODO: If the parent_page is kernel page, then return immediately. */
	if (is_kernel_vaddr(va))
		return true;

	/* 2. Resolve VA from the parent's page map level 4. */
	parent_page = pml4_get_page (parent->pml4, va);
	if (parent_page == NULL)
		return false;

	/* 3. TODO: Allocate new PAL_USER page for the child and set result to
	 *    TODO: NEWPAGE. */
	newpage = palloc_get_page(PAL_USER);
	if (newpage == NULL)
		return false;

	/* 4. TODO: Duplicate parent's page to the new page and
	 *    TODO: check whether parent's page is writable or not (set WRITABLE
	 *    TODO: according to the result). */
	memcpy(newpage, parent_page, PGSIZE);
	writable = is_writable(pte);

	/* 5. Add new page to child's page table at address VA with WRITABLE
	 *    permission. */
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
		/* 6. TODO: if fail to insert page, do error handling. */
		return false;
	}
	return true;
}
#endif

struct dict_elem{
	uintptr_t key;
	uintptr_t value;
};
// dup_count 말고 map_idx가 더 lgtm

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
static void
__do_fork (void *aux) {
	struct intr_frame if_;
	struct thread *parent = (struct thread *) aux;
	struct thread *current = thread_current ();
	/* TODO: somehow pass the parent_if. (i.e. process_fork()'s if_) */
	struct intr_frame *parent_if;
	bool succ = true;

	parent_if = &parent->parent_if;

	/* 1. Read the cpu context to local stack. */
	memcpy (&if_, parent_if, sizeof (struct intr_frame));

	/* 2. Duplicate PT */
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
		goto error;

	process_activate (current);

#ifdef VM
	supplemental_page_table_init (&current->spt);
	if (!supplemental_page_table_copy (&current->spt, &parent->spt))
		goto error;
#else
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent))
		goto error;
#endif

    // printf("__debug : do_fork check 3\n");
	/* TODO: Your code goes here.
	 * TODO: Hint) To duplicate the file object, use `file_duplicate`
	 * TODO:       in include/filesys/file.h. Note that parent should not return
	 * TODO:       from the fork() until this function successfully duplicates
	 * TODO:       the resources of parent.*/

	if (parent->fd_idx == FDCOUNT_LIMIT)
		goto error;

	const int DICTLEN = 100;
	struct dict_elem dup_file_dict[100];
	int dup_idx = 0;
	
	// current->fd_table[0] = parent->fd_table[0];
	// current->fd_table[1] = parent->fd_table[1];

	for(int i = 0; i < FDCOUNT_LIMIT; i++){
		struct file *f = parent->fd_table[i];
		if (f==NULL) continue;
		
		bool is_exist = false;
		for (int j = 0; j <= dup_idx; j++){
			if (dup_file_dict[j].key == f){
				current->fd_table[i] = dup_file_dict[j].value;
				is_exist = true;
				break;
			}
		}
		if (is_exist)
			continue;
		
		struct file *new_f;
		if (f>2)
			new_f = file_duplicate(f);
		else
			new_f = f;

		current->fd_table[i] = new_f;

		if(dup_idx<DICTLEN){
			dup_file_dict[dup_idx].key = f;
			dup_file_dict[dup_idx].value = new_f;
			dup_idx ++;
		}
	}

	current->fd_idx = parent->fd_idx;
	sema_up(&current->fork_sema);

	if_.R.rax = 0; // 반환값 (자식프로세스가 0을 반환해야 함.)
	// process_init ();

	/* Finally, switch to the newly created process. */
	if (succ)
		do_iret (&if_);
error:
	current->exit_status = TID_ERROR; // 없어도될듯? 실험
	sema_up(&current->fork_sema);
	exit(TID_ERROR);
	// thread_exit ();
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
int
process_exec (void *f_name) { // 실험 
	char *file_name = f_name;
	bool success;

	/* We cannot use the intr_frame in the thread structure.
	 * This is because when current thread rescheduled,
	 * it stores the execution information to the member. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* We first kill the current context */
	process_cleanup ();
#ifdef VM
    supplemental_page_table_init(&thread_current()->spt);
#endif
	// memset(&_if, 0, sizeof _if); // Project 2 (argument passing 관련 변경) // 이거 삭제

	/* And then load the binary */
	success = load (file_name, &_if); // Project 2 (argument passing 관련 변경)

	/* If load failed, quit. */
	
	if (!success){
		palloc_free_page (file_name); 
		return -1;
	}

	/* argument stack 함수 위치가 여기여야 할까? */

	// hex_dump(_if.rsp, _if.rsp, USER_STACK - _if.rsp, true); //Project 2 (argument passing 관련 변경) // KERN_BASE -> USER_STACK

	/* Start switched process. */
	do_iret (&_if);
	NOT_REACHED ();
}

// Project 2 (argument passing 관련 변경)
void argument_stack(char **argv, int argc, struct intr_frame *if_){
	char *arg_address[128];

	// part A: word-align 전까지
	for(int i = argc - 1 ; i >= 0; i--){
		int arg_i_len = strlen(argv[i]) + 1; // include sentinel(\0)
		if_->rsp = if_->rsp - (arg_i_len); // 인자 크기만큼 스택을 늘려줌
		memcpy(if_->rsp, argv[i], arg_i_len); // 늘려준 공간에 해당 인자를 복사
		arg_address[i] = if_->rsp; // arg_address 에 위 인자를 복사해준 주소값을 저장
	}

	// part B : word-align (8의 배수)
	while(if_ -> rsp % 8 != 0){
		if_->rsp--;
		*(uint8_t *) if_->rsp = 0;
	}

	// part C: word-align 이후 (깃북 argv[4]~argv[0]의 주소를 data로 넣기)
	for(int i = argc; i>=0; i--){
		if_->rsp = if_->rsp - 8;
		if(i==argc)
			memset(if_->rsp, 0, sizeof(char **));
		else
			memcpy(if_->rsp, &arg_address[i], sizeof(char **));
	}

	// part D: rdi, rsi 세팅

	if_->R.rdi = argc;
	if_->R.rsi = if_->rsp; // 굳이 -8 하고나서 +8 안하고, 그냥 여기서 세팅하기

	// part E: 마지막줄 (fake address)
	if_->rsp = if_->rsp - 8;
	memset(if_->rsp, 0, sizeof(void *));
}


/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */
int
process_wait (tid_t child_tid UNUSED) {
	/* XXX: Hint) The pintos exit if process_wait (initd), we recommend you
	 * XXX:       to add infinite loop here before
	 * XXX:       implementing the process_wait. */
	// struct thread *curr = thread_current();
	struct thread *child = get_child(child_tid);

	if (child == NULL)
		return -1;

	sema_down(&child->wait_sema);
	int exit_status = child->exit_status;
	list_remove(&child->child_elem);
	sema_up(&child->free_sema);
	
	return exit_status;
}

/* Exit the process. This function is called by thread_exit (). */
void
process_exit (void) {
	struct thread *curr = thread_current ();
	/* TODO: Your code goes here.
	 * TODO: Implement process termination message (see
	 * TODO: project2/process_termination.html).
	 * TODO: We recommend you to implement process resource cleanup here. */
	
	// struct file **curr_fd_table = curr->fd_table;
	// for (int i = curr->fd_idx; i >= 2; i--){
	// 	process_close_file(i);
	// }
	for (int i = 0; i < FDCOUNT_LIMIT; i++){ //curr->fd_idx 까지만 해도 가능할듯
		close(i);
	}

	// curr->fd_idx = 2;

	palloc_free_multiple(curr->fd_table, FDT_PAGES);

	file_close(curr->running); // denying writes to executable

	process_cleanup ();

	sema_up(&curr->wait_sema);

	sema_down(&curr->free_sema);
}

/* Free the current process's resources. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif

	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		/* Correct ordering here is crucial.  We must set
		 * cur->pagedir to NULL before switching page directories,
		 * so that a timer interrupt can't switch back to the
		 * process page directory.  We must activate the base page
		 * directory before destroying the process's page
		 * directory, or our active page directory will be one
		 * that's been freed (and cleared). */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
void
process_activate (struct thread *next) {
	/* Activate thread's page tables. */
	pml4_activate (next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	tss_update (next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
#define EI_NIDENT 16

#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
struct ELF64_hdr {
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* Abbreviations */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
static bool
load (const char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;

	// Project 2 (argument passing 관련 변경)
	char *arg_list[128];
	char *token, *save_ptr;
	int token_count = 0;
	
	token = strtok_r(file_name, " ", &save_ptr);
	arg_list[token_count] = token;

	while(token!=NULL){
		token = strtok_r(NULL, " ", &save_ptr);
		token_count++;
		arg_list[token_count] = token;
	}

	/* Allocate and activate page directory. */
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
		goto done;
	process_activate (thread_current ());

	/* Open executable file. */
	file = filesys_open (file_name);
	if (file == NULL) {
		printf ("load: %s: open failed\n", file_name);
		goto done;
	}

	/* denying writes to executable */
	/* 실행 중인 프로세스가 파일을 열었을 때는, write(덮어쓰기)가 발생하지 않도록 방지해주는 역할 */
	t->running = file;
	file_deny_write(file);

	/* Read and verify executable header. */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr 
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7) // 7바이트 비교한거가 다르거나 
			|| ehdr.e_type != 2 // 실행파일(type 2)이 아니거나
			|| ehdr.e_machine != 0x3E // amd64가 아니거나
			|| ehdr.e_version != 1 // object 파일 버전이 현재버전(1)이 아니거나
			|| ehdr.e_phentsize != sizeof (struct Phdr) // 프로그램 해더 테이블의 한 엔트리 크기가, Phdr 구조체 사이즈와 다르거나
			|| ehdr.e_phnum > 1024) { //  프로그램 헤더 테이블의 엔트리수가 1024보다 많거나
		printf ("load: %s: error loading executable\n", file_name); // 위 중 하나라도 해당하면, error
		goto done;
	}

	/* Read program headers. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;
		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
				/* Ignore this segment. */
				break;
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			case PT_LOAD:
				if (validate_segment (&phdr, file)) {
					bool writable = (phdr.p_flags & PF_W) != 0;
					uint64_t file_page = phdr.p_offset & ~PGMASK;
					uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
					uint64_t page_offset = phdr.p_vaddr & PGMASK;
					uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
						/* Normal segment.
						 * Read initial part from disk and zero the rest. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* Entirely zero.
						 * Don't read anything from disk. */
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}
					if (!load_segment (file, file_page, (void *) mem_page,
								read_bytes, zero_bytes, writable))
						goto done;
				}
				else
					goto done;
				break;
		}
	}

	/* Set up stack. */
	if (!setup_stack (if_))
		goto done;

	/* Start address. */
	if_->rip = ehdr.e_entry;

	/* TODO: Your code goes here.
	 * TODO: Implement argument passing (see project2/argument_passing.html). */

	 // Project 2 (argument passing 관련 변경)
	argument_stack(arg_list, token_count, if_);
	success = true;

done:
	/* We arrive here whether the load is successful or not. */
	// file_close (file);
	return success;
}


/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset and p_vaddr must have the same page offset. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	if (phdr->p_memsz == 0)
		return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* It's okay. */
	return true;
}



#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */

/* load() helpers. */
static bool install_page (void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
	}
	return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else // 살려!
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */

/*** team 7 ***/
static bool
lazy_load_segment (struct page *page, void *aux) {
	/* TODO: Load the segment from the file */
    struct file *running_file = thread_current()->running;
    struct lazy_aux *l_aux = aux;
    
	/* TODO: This called when the first page fault occurs on address VA. */
    file_seek(running_file, l_aux->ofs);
    if (file_read(running_file, page->frame->kva, l_aux->page_read_bytes) != l_aux->page_read_bytes){
        // palloc_free_page(page->frame->kva); /*** debugging ddalgui ***/
        return false;
    }
    memset(page->frame->kva + l_aux->page_read_bytes, 0, l_aux->page_zero_bytes);
    free(aux);
    return true;
	/* TODO: VA is available when calling this function. */
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
/*** team 7 ***/
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);


	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

        struct lazy_aux *aux = calloc(1, sizeof(struct lazy_aux));
        ASSERT (aux != NULL);

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
        aux->page_read_bytes = page_read_bytes;
        aux->page_zero_bytes = page_zero_bytes;
        aux->ofs = ofs;
    
		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
					writable, lazy_load_segment, aux))
			return false;

		/* Advance. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
        ofs += page_read_bytes; // team 7
	}
    // free(aux);
	return true;
}

/* team 7 */
static bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
    struct page *page = NULL;
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);
	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: Your code goes here */

    if (!vm_alloc_page(VM_MARKER_0 | VM_ANON, stack_bottom, 1))
        return false;

    success = vm_claim_page(stack_bottom);
    
    if (success) {
        if_->rsp = USER_STACK;
		thread_current()->stack_bottom = stack_bottom;
    }

	return success;
}
#endif /* VM */

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
/*** team 7 ***/
struct thread * get_child(int pid){
	struct thread *curr = thread_current(); // 부모 쓰레드
	struct list *curr_child_list = &curr->child_list; // 부모의 자식 리스트
	struct list_elem *e;
	for (e = list_begin(curr_child_list); e != list_end(curr_child_list); e = list_next(e)){
		struct thread *now = list_entry(e, struct thread, child_elem);
		if (now->tid == pid)
			return now;
	}
	return NULL;
}

