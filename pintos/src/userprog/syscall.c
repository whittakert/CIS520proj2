#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "userprog/process.h"
#include "userprog/pagedir.h"
#include "devices/input.h"
#include "devices/shutdown.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

static int sys_halt (void);
static int sys_exit (int status);
static int sys_exec (const char *ufile);
static int sys_wait (tid_t);
static int sys_create (const char *ufile, unsigned initial_size);
static int sys_remove (const char *ufile);
static int sys_open (const char *ufile);
static int sys_filesize (int handle);
static int sys_read (int handle, void *udst_, unsigned size);
static int sys_write (int handle, void *usrc_, unsigned size);
static int sys_seek (int handle, unsigned position);
static int sys_tell (int handle);
static int sys_close (int handle);
 
static void syscall_handler (struct intr_frame *);
static void copy_in (void *, const void *, size_t);
 
/* Serializes file system operations. */
static struct lock fs_lock;
 
void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init (&fs_lock);
}
 
/* System call handler. */

static void
syscall_handler (struct intr_frame *f UNUSED)
{

int syscall_num = *(int*)f->esp;
int arg_1 = *((int*)f->esp + 1);
int arg_2 = *((int*)f->esp + 2);
int arg_3 = *((int*)f->esp + 3);

void* krnl_addr;
char* krnl_str;

switch( syscall_num )
{
	case SYS_HALT:
		sys_halt();
		break;

	case SYS_EXIT:
		sys_exit( arg_1 );
		break;

	case SYS_EXEC:
		f->eax = sys_exec( (char*)arg_1 );
		break;

	case SYS_WAIT:
		sys_wait( (tid_t)arg_1 );
		break;

	case SYS_CREATE:
		f->eax = sys_create( (char*)arg_1, (unsigned)arg_2 );
		break;

	case SYS_REMOVE:
		f->eax = sys_remove( (char*)arg_1 );
		break;

	case SYS_OPEN:
		f->eax = sys_open( (char*)arg_1 );
		break;

	case SYS_FILESIZE:
		f->eax = sys_filesize( arg_1 );
		break;

	case SYS_READ:
		f->eax = sys_read( arg_1, (void*)arg_2, (unsigned)arg_3 );
		break;

	case SYS_WRITE:
		f->eax = sys_write( arg_1, (void*)arg_2, (unsigned)arg_3 );
		break;

	case SYS_SEEK:
		sys_seek( arg_1, (unsigned)arg_2 );
		break;

	case SYS_TELL:
		f->eax = sys_tell( arg_1 );
		break;

	case SYS_CLOSE:
		sys_close( arg_1 );
		break;

	default:
		break;

}
}

/* Returns true if UADDR is a valid, mapped user address,
   false otherwise. */
static bool
verify_user (const void *uaddr) 
{
  return (uaddr < PHYS_BASE
          && pagedir_get_page (thread_current ()->pagedir, uaddr) != NULL);
}
 
/* Copies a byte from user address USRC to kernel address DST.
   USRC must be below PHYS_BASE.
   Returns true if successful, false if a segfault occurred. */
static inline bool
get_user (uint8_t *dst, const uint8_t *usrc)
{
  int eax;
  asm ("movl $1f, %%eax; movb %2, %%al; movb %%al, %0; 1:"
       : "=m" (*dst), "=&a" (eax) : "m" (*usrc));
  return eax != 0;
}
 
/* Writes BYTE to user address UDST.
   UDST must be below PHYS_BASE.
   Returns true if successful, false if a segfault occurred. */
static inline bool
put_user (uint8_t *udst, uint8_t byte)
{
  int eax;
  asm ("movl $1f, %%eax; movb %b2, %0; 1:"
       : "=m" (*udst), "=&a" (eax) : "q" (byte));
  return eax != 0;
}
 
/* Copies SIZE bytes from user address USRC to kernel address
   DST.
   Call thread_exit() if any of the user accesses are invalid. */
static void
copy_in (void *dst_, const void *usrc_, size_t size) 
{
  uint8_t *dst = dst_;
  const uint8_t *usrc = usrc_;
 
  for (; size > 0; size--, dst++, usrc++) 
    if (usrc >= (uint8_t *) PHYS_BASE || !get_user (dst, usrc)) 
      thread_exit ();
}
 
/* Creates a copy of user string US in kernel memory
   and returns it as a page that must be freed with
   palloc_free_page().
   Truncates the string at PGSIZE bytes in size.
   Call thread_exit() if any of the user accesses are invalid. */
static char *
copy_in_string (const char *us) 
{
  char *ks;
  size_t length;
 
  ks = palloc_get_page (0);
  if (ks == NULL)
    thread_exit ();
 
  for (length = 0; length < PGSIZE; length++)
    {
      if (us >= (char *) PHYS_BASE || !get_user (ks + length, us++)) 
        {
          palloc_free_page (ks);
          thread_exit (); 
        }
      if (ks[length] == '\0')
        return ks;
    }
  ks[PGSIZE - 1] = '\0';
  return ks;
}
 
/* Halt system call. */
static int
sys_halt (void)
{
  shutdown_power_off ();
}
 
/* Exit system call. */
static int
sys_exit (int exit_code) 
{
  thread_current ()->wait_status->exit_code = exit_code;
  thread_exit ();
  NOT_REACHED ();
}
 
/* Exec system call. */
static int
sys_exec (const char *ufile) 
{
	return process_execute (ufile);
}
 
/* Wait system call. */
static int
sys_wait (tid_t child) 
{
	return process_wait(child);
}
 
/* Create system call. */
static int
sys_create (const char *ufile, unsigned initial_size) 
{
  char *kfile = copy_in_string (ufile);
	bool created;
	lock_acquire (&fs_lock);
  created = filesys_create(kfile, (off_t)initial_size);
  lock_release (&fs_lock);
  palloc_free_page (kfile);
	return created;
}
 
/* Remove system call. */
static int
sys_remove (const char *ufile) 
{
  char *kfile = copy_in_string (ufile);
	bool removed;
	lock_acquire (&fs_lock);
  removed = filesys_remove(kfile);
  lock_release (&fs_lock);
  palloc_free_page (kfile);
	return removed;
}
 
/* A file descriptor, for binding a file handle to a file. */
struct file_descriptor
  {
    struct list_elem elem;      /* List element. */
    struct file *file;          /* File. */
    int handle;                 /* File handle. */
  };
 
/* Open system call. */
static int
sys_open (const char *ufile) 
{
  char *kfile = copy_in_string (ufile);
  struct file_descriptor *fd;
  int handle = -1;
 
  fd = malloc (sizeof *fd);
  if (fd != NULL)
    {
      lock_acquire (&fs_lock);
      fd->file = filesys_open (kfile);
      if (fd->file != NULL)
        {
          struct thread *cur = thread_current ();
          handle = fd->handle = cur->next_handle++;
          list_push_front (&cur->fds, &fd->elem);
        }
      else 
        free (fd);
      lock_release (&fs_lock);
    }
  palloc_free_page (kfile);
  return handle;
}
 
/* Returns the file descriptor associated with the given handle.
   Terminates the process if HANDLE is not associated with an
   open file. */
static struct file_descriptor *
lookup_fd (int handle)
{
	struct list* file_list = &(thread_current()->fds);
	struct list_elem* temp;
	temp = list_head ( file_list );
	while ( temp != list_end( file_list ) )
	{
		struct file_descriptor *fd = list_entry(temp, struct file_descriptor, elem);
		if ( fd->handle == handle )
		{
			return fd;
		}
		else
		temp = list_next ( temp ); 
	}
	thread_exit();
}
 
/* Filesize system call. */
static int
sys_filesize (int handle) 
{
	int size;
	struct file_descriptor *fd = lookup_fd (handle);
  lock_acquire (&fs_lock);
	size= file_length(fd->file);
  lock_release (&fs_lock);
	return size;
}
 
/* Read system call. */
static int
sys_read (int handle, void *udst_, unsigned size) 
{
  uint8_t *udst = udst_;
  struct file_descriptor *fd = NULL;
  int bytes_read = 0;

  /* Lookup up file descriptor. */
  if (handle != STDOUT_FILENO)
    fd = lookup_fd (handle);

  lock_acquire (&fs_lock);
  while (size > 0) 
    {
      /* How much bytes to read from this page? */
      size_t page_left = PGSIZE - pg_ofs (udst);
      size_t read_amt = size < page_left ? size : page_left;
      off_t retval;

      /* Check that we can touch this user page. */
      if (!verify_user (udst)) 
        {
          lock_release (&fs_lock);
          thread_exit ();
        }

      retval = file_read (fd->file, udst, read_amt);
      if (retval < 0) 
        {
          if (bytes_read == 0)
            bytes_read = -1;
          break;
        }
      bytes_read += retval;

      /* If it was a short read we're done. */
      if (retval != (off_t) read_amt)
        break;

      /* Advance. */
      udst += retval;
      size -= retval;
    }
  lock_release (&fs_lock);
  return bytes_read;
}
 
/* Write system call. */
static int
sys_write (int handle, void *usrc_, unsigned size) 
{
  uint8_t *usrc = usrc_;
  struct file_descriptor *fd = NULL;
  int bytes_written = 0;

  /* Lookup up file descriptor. */
  if (handle != STDOUT_FILENO)
    fd = lookup_fd (handle);

  lock_acquire (&fs_lock);
  while (size > 0) 
    {
      /* How much bytes to write to this page? */
      size_t page_left = PGSIZE - pg_ofs (usrc);
      size_t write_amt = size < page_left ? size : page_left;
      off_t retval;

      /* Check that we can touch this user page. */
      if (!verify_user (usrc)) 
        {
          lock_release (&fs_lock);
          thread_exit ();
        }

      /* Do the write. */
      if (handle == STDOUT_FILENO)
        {
          putbuf ((char*)usrc, write_amt);
          retval = write_amt;
        }
      else
        retval = file_write (fd->file, usrc, write_amt);
      if (retval < 0) 
        {
          if (bytes_written == 0)
            bytes_written = -1;
          break;
        }
      bytes_written += retval;

      /* If it was a short write we're done. */
      if (retval != (off_t) write_amt)
        break;

      /* Advance. */
      usrc += retval;
      size -= retval;
    }
  lock_release (&fs_lock);
 
  return bytes_written;
}
 
/* Seek system call. */
static int
sys_seek (int handle, unsigned position) 
{
	struct file_descriptor *fd = lookup_fd (handle);
	if (fd == NULL) thread_exit();
	lock_acquire (&fs_lock);
	file_seek(fd->file, position);
	lock_release (&fs_lock);
}
 
/* Tell system call. */
static int
sys_tell (int handle) 
{
	struct file_descriptor *fd = lookup_fd (handle);
	if (fd == NULL) thread_exit();	
	unsigned pos;
	lock_acquire (&fs_lock);
	pos = (unsigned)file_tell(fd->file);
	lock_release (&fs_lock);
	return pos;
}
 
/* Close system call. */
static int
sys_close (int handle) 
{
	struct file_descriptor *fd = lookup_fd (handle);
	if (fd == NULL) thread_exit(); 
	lock_acquire (&fs_lock);
	file_close(fd->file);
	lock_release (&fs_lock);
}
 
/* On thread exit, close all open files. */
void
syscall_exit (void) 
{
/* Add code */
  return;
}
