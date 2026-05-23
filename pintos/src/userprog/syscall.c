#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "userprog/process.h"
#include "userprog/pagedir.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "devices/shutdown.h"
#include "devices/input.h"

static void syscall_handler (struct intr_frame *);
static void check_ptr (const void *ptr);
static void check_str (const char *str);
static void check_buf (const void *buf, unsigned size);
static int get_arg_int (struct intr_frame *f, int n);

static struct lock fs_lock;

void
syscall_init (void)
{
  lock_init (&fs_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
check_ptr (const void *ptr)
{
  if (ptr == NULL || !is_user_vaddr (ptr)
      || pagedir_get_page (thread_current ()->pagedir, ptr) == NULL)
    {
      if (thread_current ()->my_info != NULL)
        thread_current ()->my_info->exit_status = -1;
      thread_exit ();
    }
}

static void
check_str (const char *str)
{
  check_ptr (str);
  while (*(char *) str != '\0')
    {
      str++;
      check_ptr (str);
    }
}

static void
check_buf (const void *buf, unsigned size)
{
  unsigned i;
  for (i = 0; i < size; i++)
    check_ptr ((const char *) buf + i);
}

static int
get_arg_int (struct intr_frame *f, int n)
{
  int *ptr = (int *) f->esp + 1 + n;
  check_buf (ptr, 4);
  return *ptr;
}

static struct fd_entry *
get_fd_entry (int fd)
{
  struct thread *cur = thread_current ();
  struct list_elem *e;
  for (e = list_begin (&cur->fd_list); e != list_end (&cur->fd_list);
       e = list_next (e))
    {
      struct fd_entry *fde = list_entry (e, struct fd_entry, elem);
      if (fde->fd == fd)
        return fde;
    }
  return NULL;
}

static void
syscall_handler (struct intr_frame *f)
{
  check_ptr (f->esp);
  int syscall_num = *(int *) f->esp;

  switch (syscall_num)
    {
    case SYS_HALT:
      shutdown_power_off ();
      break;

    case SYS_EXIT:
      {
        int status = get_arg_int (f, 0);
        struct thread *cur = thread_current ();
        if (cur->my_info != NULL)
          cur->my_info->exit_status = status;
        thread_exit ();
        break;
      }

    case SYS_EXEC:
      {
        const char *cmd = (const char *) get_arg_int (f, 0);
        check_str (cmd);
        tid_t tid = process_execute (cmd);
        f->eax = tid;
        break;
      }

    case SYS_WAIT:
      {
        tid_t pid = get_arg_int (f, 0);
        f->eax = process_wait (pid);
        break;
      }

    case SYS_CREATE:
      {
        const char *file = (const char *) get_arg_int (f, 0);
        unsigned initial_size = get_arg_int (f, 1);
        check_str (file);
        lock_acquire (&fs_lock);
        bool ok = filesys_create (file, initial_size);
        lock_release (&fs_lock);
        f->eax = ok;
        break;
      }

    case SYS_REMOVE:
      {
        const char *file = (const char *) get_arg_int (f, 0);
        check_str (file);
        lock_acquire (&fs_lock);
        bool ok = filesys_remove (file);
        lock_release (&fs_lock);
        f->eax = ok;
        break;
      }

    case SYS_OPEN:
      {
        const char *file = (const char *) get_arg_int (f, 0);
        check_str (file);
        lock_acquire (&fs_lock);
        struct file *fp = filesys_open (file);
        lock_release (&fs_lock);
        if (fp == NULL)
          {
            f->eax = -1;
            break;
          }
        struct fd_entry *fde = malloc (sizeof (struct fd_entry));
        if (fde == NULL)
          {
            file_close (fp);
            f->eax = -1;
            break;
          }
        struct thread *cur = thread_current ();
        fde->fd = cur->next_fd++;
        fde->file = fp;
        list_push_back (&cur->fd_list, &fde->elem);
        f->eax = fde->fd;
        break;
      }

    case SYS_FILESIZE:
      {
        int fd = get_arg_int (f, 0);
        struct fd_entry *fde = get_fd_entry (fd);
        if (fde == NULL)
          { f->eax = -1; break; }
        lock_acquire (&fs_lock);
        f->eax = file_length (fde->file);
        lock_release (&fs_lock);
        break;
      }

    case SYS_READ:
      {
        int fd = get_arg_int (f, 0);
        void *buf = (void *) get_arg_int (f, 1);
        unsigned size = get_arg_int (f, 2);
        check_buf (buf, size);
        if (fd == 0)
          {
            unsigned i;
            for (i = 0; i < size; i++)
              ((char *) buf)[i] = input_getc ();
            f->eax = size;
          }
        else
          {
            struct fd_entry *fde = get_fd_entry (fd);
            if (fde == NULL)
              { f->eax = -1; break; }
            lock_acquire (&fs_lock);
            f->eax = file_read (fde->file, buf, size);
            lock_release (&fs_lock);
          }
        break;
      }

    case SYS_WRITE:
      {
        int fd = get_arg_int (f, 0);
        const void *buf = (const void *) get_arg_int (f, 1);
        unsigned size = get_arg_int (f, 2);
        check_buf (buf, size);
        if (fd == 1)
          {
            putbuf (buf, size);
            f->eax = size;
          }
        else
          {
            struct fd_entry *fde = get_fd_entry (fd);
            if (fde == NULL)
              { f->eax = -1; break; }
            lock_acquire (&fs_lock);
            f->eax = file_write (fde->file, buf, size);
            lock_release (&fs_lock);
          }
        break;
      }

    case SYS_SEEK:
      {
        int fd = get_arg_int (f, 0);
        unsigned position = get_arg_int (f, 1);
        struct fd_entry *fde = get_fd_entry (fd);
        if (fde != NULL)
          {
            lock_acquire (&fs_lock);
            file_seek (fde->file, position);
            lock_release (&fs_lock);
          }
        break;
      }

    case SYS_TELL:
      {
        int fd = get_arg_int (f, 0);
        struct fd_entry *fde = get_fd_entry (fd);
        if (fde == NULL)
          { f->eax = -1; break; }
        lock_acquire (&fs_lock);
        f->eax = file_tell (fde->file);
        lock_release (&fs_lock);
        break;
      }

    case SYS_CLOSE:
      {
        int fd = get_arg_int (f, 0);
        struct fd_entry *fde = get_fd_entry (fd);
        if (fde != NULL)
          {
            lock_acquire (&fs_lock);
            file_close (fde->file);
            lock_release (&fs_lock);
            list_remove (&fde->elem);
            free (fde);
          }
        break;
      }

    default:
      if (thread_current ()->my_info != NULL)
        thread_current ()->my_info->exit_status = -1;
      thread_exit ();
      break;
    }
}
