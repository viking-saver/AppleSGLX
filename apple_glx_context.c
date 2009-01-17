/* 
 Copyright (c) 2008 Apple Inc.
 
 Permission is hereby granted, free of charge, to any person
 obtaining a copy of this software and associated documentation files
 (the "Software"), to deal in the Software without restriction,
 including without limitation the rights to use, copy, modify, merge,
 publish, distribute, sublicense, and/or sell copies of the Software,
 and to permit persons to whom the Software is furnished to do so,
 subject to the following conditions:
 
 The above copyright notice and this permission notice shall be
 included in all copies or substantial portions of the Software.
 
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 NONINFRINGEMENT.  IN NO EVENT SHALL THE ABOVE LISTED COPYRIGHT
 HOLDER(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 DEALINGS IN THE SOFTWARE.
 
 Except as contained in this notice, the name(s) of the above
 copyright holders shall not be used in advertising or otherwise to
 promote the sale, use or other dealings in this Software without
 prior written authorization.
*/

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <assert.h>
#include <pthread.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <GL/glx.h>
#include <OpenGL/CGLTypes.h>
#include <OpenGL/CGLCurrent.h>
#include <OpenGL/OpenGL.h>

#include "apple_glx_context.h"
#include "appledri.h"
#include "apple_visual.h"
#include "apple_cgl.h"
#include "apple_glx_drawable.h"


static pthread_mutex_t context_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * This should be locked on creation and destruction of the 
 * apple_glx_contexts.
 *
 * It's also locked when the surface_notify_handler is searching
 * for a uid associated with a surface.
 */
static struct apple_glx_context *context_list = NULL;

/* This guards the context_list above. */
static void lock_context_list(void) {
    if(pthread_mutex_lock(&context_lock)) {
	perror("pthread_mutex_lock");
	abort();
    }
}

static void unlock_context_list(void) {
    if(pthread_mutex_unlock(&context_lock)) {
	perror("pthread_mutex_unlock");
	abort();
    }
}


/* This creates an apple_private_context struct.  
 *
 * It's typically called to save the struct in a GLXContext.
 *
 * This is also where the CGLContextObj is created, and the CGLPixelFormatObj.
 */
void apple_glx_create_context(void **ptr, Display *dpy, int screen, 
			  const void *mode, void *sharedContext) {
    struct apple_glx_context *ac;
    struct apple_glx_context *sharedac = sharedContext;
    CGLError error;

    ac = malloc(sizeof *ac);
    if(NULL == ac) {
	perror("malloc");
	abort();   
    }
    
    ac->context_obj = NULL;
    ac->pixel_format_obj = NULL;
    ac->drawable = NULL;
    ac->thread_id = pthread_self();
    ac->screen = screen;
    
    apple_visual_create_pfobj(&ac->pixel_format_obj, mode, 
			      &ac->double_buffered);
    
    error = apple_cgl.create_context(ac->pixel_format_obj, 
				     sharedac ? sharedac->context_obj : NULL,
				     &ac->context_obj);
    if(error) {
	fprintf(stderr, "error: %s\n", apple_cgl.error_string(error));
	abort();
    }

    lock_context_list();

    if(context_list) {
	context_list->previous = ac;
    }

    ac->previous = NULL;
    ac->next = context_list;
    context_list = ac;

    unlock_context_list();

    *ptr = ac;
}

void apple_glx_destroy_context(void **ptr, Display *dpy) {
    struct apple_glx_context *ac = *ptr;

    if(NULL == ac)
	return;

    if(apple_cgl.set_current_context(NULL)) {
	abort();
    }

    /* Remove ac from the context_list as soon as possible. */
    lock_context_list();

    if(ac->previous) {
	ac->previous->next = ac->next;
    } else {
	context_list = ac->next;
    }

    if (ac->next) {
	ac->next->previous = ac->previous;
    }

    unlock_context_list();


    if(apple_cgl.clear_drawable(ac->context_obj)) {
	abort();
    }

    
    /*
     * This causes surface_notify_handler to be called in apple_glx.c... 
     * We can NOT have a lock held at this point.  It would result in 
     * an abort due to an attempted deadlock.  This is why we earlier
     * removed the ac pointer from the double-linked list.
     */
    if(ac->drawable) {
	Drawable drawable = ac->drawable->drawable;
	
	if(apple_glx_destroy_drawable(ac->drawable)) {
	    /* 
	     * The drawable has no more references, so we can destroy
	     * the surface. 
	     */
	   XAppleDRIDestroySurface(dpy, ac->screen, drawable);
	}
    }

    if(apple_cgl.destroy_pixel_format(ac->pixel_format_obj)) {
	abort();
    }

    if(apple_cgl.destroy_context(ac->context_obj)) {
	abort();
    }
    
    free(ac);

    *ptr = NULL;
    
    apple_glx_garbage_collect_drawables(dpy);
}

#if 0
static bool setup_drawable(struct apple_glx_drawable *agd) {
    printf("buffer path %s\n", agd->path);
      
    agd->fd = shm_open(agd->path, O_RDWR, 0);

    if(-1 == agd->fd) {
	perror("open");
	return true;
    }
        
    agd->row_bytes = agd->width * /*TODO don't hardcode 4*/ 4;
    agd->buffer_length = agd->row_bytes * agd->height;

    printf("agd->width %d agd->height %d\n", agd->width, agd->height);

    agd->buffer = mmap(NULL, agd->buffer_length, PROT_READ | PROT_WRITE,
		       MAP_FILE | MAP_SHARED, agd->fd, 0);

    if(MAP_FAILED == agd->buffer) {
	perror("mmap");
	close(agd->fd);
	agd->fd = -1;
	return true;
    }

    return false;
}
#endif

/* Return true if an error occured. */
/* TODO handle the readable GLXDrawable...? STUDY */

bool apple_glx_make_current_context(Display *dpy, void *ptr, GLXDrawable drawable) {
    struct apple_glx_context *ac = ptr;
    xp_error error;
    struct apple_glx_drawable *agd = NULL;
    CGLError cglerr;

    assert(NULL != dpy);
    assert(NULL != ac);
  
    if(None == drawable) {
	if(apple_cgl.clear_drawable(ac->context_obj))
	    return true;

	if(apple_cgl.set_current_context(ac->context_obj))
	    return true;

	return false;
    }

    /* Release the reference to the old drawable. */
    if(ac->drawable) {
	apple_glx_release_drawable(ac->drawable);
	ac->drawable = NULL;
    }

    agd = apple_glx_find_drawable(dpy, drawable);

    if(NULL == agd) {
	if(apple_glx_create_drawable(dpy, ac, drawable, &agd)) {
	    return true;
	}

#if 0   
	if(!XAppleDRICreateSharedBuffer(dpy, ac->screen, drawable,
					ac->double_buffered, agd->path,
					sizeof(agd->path), 
					&agd->width, 
					&agd->height)) {
	    return true;
	}

	if(setup_drawable(agd))
	    return true;
#endif
    }

    ac->drawable = agd;
  
    error = xp_attach_gl_context(ac->context_obj, agd->surface_id);

    if(error) {
	fprintf(stderr, "error: xp_attach_gl_context returned: %d\n",
		error);
	return true;
    }

    cglerr = apple_cgl.set_current_context(ac->context_obj);

    if(kCGLNoError != cglerr) {
	fprintf(stderr, "set current error: %s\n",
		apple_cgl.error_string(cglerr));
	return true;
    }
    
    //apple_cgl.clear_drawable(ac->context_obj);

#if 0
    printf("agd->buffer %p\n", agd->buffer);
#endif

#if 0
    cglerr = apple_cgl.set_off_screen(ac->context_obj,
				      agd->width, agd->height,
				      agd->row_bytes, agd->buffer);
#endif

#if 0
    if(kCGLNoError != cglerr) {
	fprintf(stderr, "set off screen error: %s\n",
		apple_cgl.error_string(cglerr));
	return true;
    }
#endif

    ac->thread_id = pthread_self();

    return false;
}

bool apple_glx_is_current_drawable(void *ptr, GLXDrawable drawable) {
    struct apple_glx_context *ac = ptr;

    assert(NULL != ac);
    
    return (ac->drawable->drawable == drawable);
}

/* Return true if an error occurred. */
bool apple_glx_get_surface_from_uid(unsigned int uid, xp_surface_id *sid,
				    CGLContextObj *contextobj) {
    struct apple_glx_context *ac;

    lock_context_list();

    for(ac = context_list; ac; ac = ac->next) {
	if(ac->drawable && ac->drawable->uid == uid) {
	    *sid = ac->drawable->surface_id;
	    *contextobj = ac->context_obj;
	    unlock_context_list();
	    return false;
	}
    }

    unlock_context_list();

    return true;
}
