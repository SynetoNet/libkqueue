/*
 * Copyright (c) 2009 Mark Heily <mark@heily.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/* To get asprintf(3) */
#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <string.h>
#include <unistd.h>

#include "sys/event.h"
#include "private.h"

static char *
kevent_filter_dump(const struct kevent *kev)
{
    char *buf;

    if ((buf = calloc(1, 64)) == NULL)
        abort();

    snprintf(buf, 64, "%d (%s)", kev->filter, filter_name(kev->filter));
    return (buf);
}

static char *
kevent_fflags_dump(const struct kevent *kev)
{
    char *buf;

#define KEVFFL_DUMP(attrib) \
    if (kev->fflags & attrib) \
    strncat(buf, #attrib" ", 64);

    if ((buf = calloc(1, 1024)) == NULL)
        abort();

    snprintf(buf, 1024, "fflags=0x%04x (", kev->fflags);
    if (kev->filter == EVFILT_VNODE) {
        KEVFFL_DUMP(NOTE_DELETE);
        KEVFFL_DUMP(NOTE_WRITE);
        KEVFFL_DUMP(NOTE_EXTEND);
        KEVFFL_DUMP(NOTE_ATTRIB);
        KEVFFL_DUMP(NOTE_LINK);
        KEVFFL_DUMP(NOTE_RENAME);
    } else if (kev->filter == EVFILT_USER) {
        KEVFFL_DUMP(NOTE_FFNOP);
        KEVFFL_DUMP(NOTE_FFAND);
        KEVFFL_DUMP(NOTE_FFOR);
        KEVFFL_DUMP(NOTE_FFCOPY);
        KEVFFL_DUMP(NOTE_TRIGGER);
    }  else {
        strncat(buf, " ", 1);
    }
    buf[strlen(buf) - 1] = ')';

#undef KEVFFL_DUMP

    return (buf);
}

static char *
kevent_flags_dump(const struct kevent *kev)
{
    char *buf;

#define KEVFL_DUMP(attrib) \
    if (kev->flags & attrib) \
	strncat(buf, #attrib" ", 64);

    if ((buf = calloc(1, 1024)) == NULL)
        abort();

    snprintf(buf, 1024, "flags=0x%04x (", kev->flags);
    KEVFL_DUMP(EV_ADD);
    KEVFL_DUMP(EV_ENABLE);
    KEVFL_DUMP(EV_DISABLE);
    KEVFL_DUMP(EV_DELETE);
    KEVFL_DUMP(EV_ONESHOT);
    KEVFL_DUMP(EV_CLEAR);
    KEVFL_DUMP(EV_EOF);
    KEVFL_DUMP(EV_ERROR);
    KEVFL_DUMP(EV_DISPATCH);
    KEVFL_DUMP(EV_RECEIPT);
    buf[strlen(buf) - 1] = ')';

#undef KEVFL_DUMP

    return (buf);
}

const char *
kevent_dump(const struct kevent *kev)
{
    char buf[512];

    snprintf(&buf[0], sizeof(buf), 
            "{ ident=%d, filter=%s, %s, %s, data=%d, udata=%p }",
            (u_int) kev->ident,
            kevent_filter_dump(kev),
            kevent_flags_dump(kev),
            kevent_fflags_dump(kev),
            (int) kev->data,
            kev->udata);

    return (strdup(buf)); /* FIXME: memory leak */
}

//DEADWOOD
static void
kevent_error(struct kevent *dst, const struct kevent *src, int data)
{
    memcpy(dst, src, sizeof(*src));
    dst->data = data;
}

// replace the inner loop of kevent_copyin() and call new knote hooks
static int
kevent_copyin_one(struct kqueue *kq, const struct kevent *src)
{
    struct knote  *kn = NULL;
    struct filter *filt;
    int rv;

    if (filter_lookup(&filt, kq, src->filter) < 0) 
        return (-1);

    //dbg_printf("src=%s\n", kevent_dump(src));

    kn = knote_lookup(filt, src->ident);
    if (kn == NULL) {
        if (src->flags & EV_ADD) {
            if ((kn = knote_new(filt)) == NULL) {
                errno = ENOENT;
                return (-1);
            }
            memcpy(&kn->kev, src, sizeof(kn->kev));
            kn->kev.flags &= ~EV_ENABLE;
            kn->kev.flags |= EV_ADD;//FIXME why?
            assert(filt->kn_create);
            if (filt->kn_create(filt, kn) < 0) {
                knote_free(filt, kn);
                errno = EFAULT;
                return (-1);
            } 
            dbg_printf("created kevent %s\n", kevent_dump(src));
            if (src->flags & EV_DISABLE) {
                kn->kev.flags |= EV_DISABLE;
                return (filt->kn_disable(filt, kn));
            }
            return (0);
        } else {
            errno = ENOENT;
            return (-1);
        }
    }

    if (src->flags & EV_DELETE) {
        rv = filt->kn_delete(filt, kn);
        knote_free(filt, kn);
        return (rv);
    } else if (src->flags & EV_DISABLE) {
        kn->kev.flags |= EV_DISABLE;
        return (filt->kn_disable(filt, kn));
    } else if (src->flags & EV_ENABLE) {
        kn->kev.flags &= ~EV_DISABLE;
        return (filt->kn_enable(filt, kn));
    } else {
        return (filt->kn_modify(filt, kn, src));
    }

    errno = EINVAL;
    return (-1);

#if DEADWOOD
    /* Special case for EVFILT_USER:
       Ignore user-generated events that are not of interest */
    if (src->fflags & NOTE_TRIGGER) {
        filter_unlock(filt);
        continue;
    }
#endif
}

/** @return number of events added to the eventlist */
static int
kevent_copyin(struct kqueue *kq, const struct kevent *src, int nchanges,
        struct kevent *eventlist, int nevents)
{
    int status, nret;

    dbg_printf("nchanges=%d nevents=%d", nchanges, nevents);

    for (nret = 0; nchanges > 0; src++, nchanges--) {

        if (kevent_copyin_one(kq, src) < 0) {
            status = errno;
            goto err_path;
        } else {
            if (src->flags & EV_RECEIPT) {
                status = 0;
                goto err_path;
            }
        }

        continue;

err_path:
        if (nevents > 0) {
            memcpy(eventlist, src, sizeof(*src));
            eventlist->data = errno;
            nevents--;
            eventlist++;
            nret++;
        } else {
            return (-1);
        }
    }

    return (nret);
}

/** @return number of events added to the eventlist */
int
DEADWOOD_kevent_copyin(struct kqueue *kq, const struct kevent *src, int nchanges,
        struct kevent *eventlist, int nevents)
{
    struct knote  *dst = NULL;
    struct filter *filt;
    int kn_alloc;
    int status, rv;

    kn_alloc = 0;
    status = 0;
    rv = 0;

    dbg_printf("nchanges=%d nevents=%d", nchanges, nevents);

    for (; nchanges > 0; src++, nchanges--) {

        status = filter_lookup(&filt, kq, src->filter);
        if (status < 0) 
            goto err_out_unlocked;

        dbg_printf("%d %s\n", nchanges, kevent_dump(src));

        /*
         * Retrieve an existing knote object, or create a new one.
         */
        kn_alloc = 0;
        dst = knote_lookup(filt, src->ident);
        if (dst == NULL) {
           if (src->flags & EV_ADD) {
               if ((dst = knote_new(filt)) == NULL) {
                   status = -ENOMEM;
                   goto err_out;
               }
               kn_alloc = 1;
           } else if (src->flags & EV_ENABLE 
                   || src->flags & EV_DISABLE
                   || src->flags & EV_DELETE) {
               status = -ENOENT;
               goto err_out;
           } else {

               /* Special case for EVFILT_USER:
                  Ignore user-generated events that are not of interest */
               if (src->fflags & NOTE_TRIGGER) {
                   continue;
               }

               /* flags == 0 or no action */
               status = -EINVAL;
               goto err_out;
           }
        }

        if (filt->kf_copyin(filt, dst, src) < 0) {
            status = -EBADMSG;
            goto err_out;
        }

        /*
         * Update the knote flags based on src->flags.
         */
        if (src->flags & EV_ENABLE)
            KNOTE_ENABLE(dst);
        if (src->flags & EV_DISABLE) 
            KNOTE_DISABLE(dst);
        if (src->flags & EV_DELETE) 
            knote_free(filt, dst);
        if (src->flags & EV_RECEIPT) {
            status = 0;
            goto err_out;
        }

        continue;

err_out:
        //DEADWOOD: filter_unlock(filt);
err_out_unlocked:
        if (status != 0 && kn_alloc)
            knote_free(filt, dst);
        if (nevents > 0) {
            kevent_error(eventlist++, src, status);
            nevents--;
            rv++;
        } else {
            return (-1);
        }
    }

    return (rv);
}

int __attribute__((visibility("default")))
kevent(int kqfd, const struct kevent *changelist, int nchanges,
        struct kevent *eventlist, int nevents,
        const struct timespec *timeout)
{
    struct kqueue *kq;
    int rv, n, nret;

    errno = 0;

    if (kqueue_gc() < 0)
        return (-1);

    kq = kqueue_lookup(kqfd);
    if (kq == NULL) {
        dbg_printf("fd lookup failed; fd=%d", kqfd);
        errno = EINVAL;
        return (-1);
    }

    /*
     * Process each kevent on the changelist.
     */
    if (nchanges) {
        rv = kevent_copyin(kq, changelist, nchanges, eventlist, nevents);
        if (rv < 0)
            return (-1); 
        if (rv > 0) {
            eventlist += rv;
            nevents -= rv;
        }
    }

    /* Determine if we need to wait for events. */
    if (nevents == 0)
        return (0);
    if (nevents > MAX_KEVENT)
        nevents = MAX_KEVENT;

    /* Handle spurious wakeups where no events are generated. */
    for (nret = 0; nret == 0; 
            nret = kevent_copyout(kq, n, eventlist, nevents)) 
    {
        /* Wait for one or more events. */
        n = kevent_wait(kq, timeout);
        if (n < 0)
            return (-1);
        if (n == 0)
            return (0);             /* Timeout */
    }

#ifdef KQUEUE_DEBUG
    dbg_printf("returning %d events", nret);
    for (n = 0; n < nret; n++) {
        dbg_printf("eventlist[%d] = %s", n, kevent_dump(&eventlist[n]));
    }
#endif /* KQUEUE_DEBUG */

    return (nret);
}
