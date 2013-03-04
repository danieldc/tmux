/* $Id$ */

/*
 * Copyright (c) 2013 Nicholas Marriott <nicm@users.sourceforge.net>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <ctype.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

#include "tmux.h"

/* Create new command queue. */
struct cmd_q *
cmdq_new(struct client *c)
{
	struct cmd_q	*cmdq;

	cmdq = xcalloc(1, sizeof *cmdq);
	cmdq->references = 1;
	cmdq->dead = 0;

	cmdq->client = c;
	cmdq->client_exit = 0;

	TAILQ_INIT(&cmdq->queue);
	cmdq->item = NULL;
	cmdq->cmd = NULL;

	return (cmdq);
}

/* Free command queue */
int
cmdq_free(struct cmd_q *cmdq)
{
	if (--cmdq->references != 0)
		return (cmdq->dead);

	cmdq_flush(cmdq);
	free(cmdq);
	return (1);
}

/* Show message from command. */
void printflike2
cmdq_print(struct cmd_q *cmdq, const char *fmt, ...)
{
	struct client	*c = cmdq->client;
	struct window	*w;
	va_list		 ap;

	va_start(ap, fmt);

	if (c == NULL)
		/* nothing */;
	else if (c->session == NULL || (c->flags & CLIENT_CONTROL)) {
		va_start(ap, fmt);
		evbuffer_add_vprintf(c->stdout_data, fmt, ap);
		va_end(ap);

		evbuffer_add(c->stdout_data, "\n", 1);
		server_push_stdout(c);
	} else {
		w = c->session->curw->window;
		if (w->active->mode != &window_copy_mode) {
			window_pane_reset_mode(w->active);
			window_pane_set_mode(w->active, &window_copy_mode);
			window_copy_init_for_output(w->active);
		}
		window_copy_vadd(w->active, fmt, ap);
	}

	va_end(ap);
}

/* Show info from command. */
void printflike2
cmdq_info(struct cmd_q *cmdq, const char *fmt, ...)
{
	struct client	*c = cmdq->client;
	va_list		 ap;
	char		*msg;

	if (options_get_number(&global_options, "quiet"))
		return;

	va_start(ap, fmt);

	if (c == NULL)
		/* nothing */;
	else if (c->session == NULL || (c->flags & CLIENT_CONTROL)) {
		va_start(ap, fmt);
		evbuffer_add_vprintf(c->stdout_data, fmt, ap);
		va_end(ap);

		evbuffer_add(c->stdout_data, "\n", 1);
		server_push_stdout(c);
	} else {
		xvasprintf(&msg, fmt, ap);
		*msg = toupper((u_char) *msg);
		status_message_set(c, "%s", msg);
		free(msg);
	}

	va_end(ap);

}

/* Show error from command. */
void printflike2
cmdq_error(struct cmd_q *cmdq, const char *fmt, ...)
{
	struct client	*c = cmdq->client;
	struct cmd	*cmd = cmdq->cmd;
	va_list		 ap;
	char		*msg, *cause;
	size_t		 msglen;

	va_start(ap, fmt);
	msglen = xvasprintf(&msg, fmt, ap);
	va_end(ap);

	if (c == NULL) {
		xasprintf(&cause, "%s:%u: %s", cmd->file, cmd->line, msg);
		ARRAY_ADD(&cfg_causes, cause);
	} else if (c->session == NULL || (c->flags & CLIENT_CONTROL)) {
		evbuffer_add(c->stderr_data, msg, msglen);
		evbuffer_add(c->stderr_data, "\n", 1);

		server_push_stderr(c);
		c->retcode = 1;
	} else {
		*msg = toupper((u_char) *msg);
		status_message_set(c, "%s", msg);
	}

	free(msg);
}

/* Print a guard line. */
int
cmdq_guard(struct cmd_q *cmdq, const char *guard)
{
	struct client	*c = cmdq->client;

	if (c == NULL || c->session == NULL)
		return 0;
	if (!(c->flags & CLIENT_CONTROL))
		return 0;

	evbuffer_add_printf(c->stdout_data, "%%%s %ld %u\n", guard,
	    (long) cmdq->time, cmdq->number);
	server_push_stdout(c);
	return 1;
}

/* Add command list to queue and begin processing if needed. */
void
cmdq_run(struct cmd_q *cmdq, struct cmd_list *cmdlist)
{
	struct client		*c;
	struct cmd_q		*cmdq_hooks;
	struct cmd_q_item	*item;
	struct cmd		*cmd;
	struct hooks		*hooks;
	struct hook		*hook_before, *hook_after;
	char			*hook_before_name, *hook_after_name;

	c = cmdq->client != NULL ? cmdq->client : NULL;
	///hooks = &global_hooks;
	hooks = (c != NULL && c->session != NULL) ? &c->session->hooks : &global_hooks;
	cmdq_append(cmdq, cmdlist);

	if (hooks == NULL)
		goto out;

	log_debug("HOOKS NOT NULL!!!");

	/* Hooks */
	TAILQ_FOREACH(item, &cmdq->queue, qentry) {
		TAILQ_FOREACH(cmd, &item->cmdlist->list, qentry) {
			/* 
			 * For the given command in this list, look to see if
			 * this has any hooks.
			 */
			xasprintf(&hook_before_name, "before-%s", cmd->entry->name);
			xasprintf(&hook_after_name, "after-%s", cmd->entry->name);
			hook_before = hooks_find(hooks, hook_before_name);
			hook_after = hooks_find(hooks, hook_after_name);

			free(hook_before_name);
			free(hook_after_name);

			/* No hooks found.  Just use the command passed in. */
			if (hook_before == NULL && hook_after == NULL) {
				log_debug("No hooks defined for session:  <<%s>>",
					(c != NULL && c->session != NULL) ?
					c->session->name : "(null)");
				goto out;
			}

			cmdq_hooks = cmdq_new(c);

			if (hook_before != NULL)
				cmdq_append(cmdq_hooks, hook_before->cmdlist);

			/* Then the command itself. */
			cmdq_append(cmdq_hooks, cmdlist);

			if (hook_after != NULL)
				cmdq_append(cmdq_hooks, hook_after->cmdlist);

			/* 
			 * We have some hooks in which case, we
			 * must recreate the cmdq, this time flushing and
			 * freeing it, and reusing the client from the
			 * original.
			 */
			cmdq_free(cmdq);
			cmdq = cmdq_hooks;

			/* 
			 * Assign the cmdq back to the client if it came from
			 * there originally.
			 */
#if 0
			if (c != NULL) {
				cmdq_free(c->cmdq);
				c->cmdq = cmdq_hooks;
			}
#endif
		}
	}
out:
	if (cmdq->item == NULL) {
		cmdq->cmd = NULL;
		cmdq_continue(cmdq);
	}
}

/* Add command list to queue. */
void
cmdq_append(struct cmd_q *cmdq, struct cmd_list *cmdlist)
{
	struct cmd_q_item	*item;

	item = xcalloc(1, sizeof *item);
	item->cmdlist = cmdlist;
	TAILQ_INSERT_TAIL(&cmdq->queue, item, qentry);
	cmdlist->references++;
}

/* Continue processing command queue. Returns 1 if finishes empty. */
int
cmdq_continue(struct cmd_q *cmdq)
{
	struct cmd_q_item	*next;
	enum cmd_retval		 retval;
	int			 empty, guard;
	char			 s[1024];

	notify_disable();

	empty = TAILQ_EMPTY(&cmdq->queue);
	if (empty)
		goto empty;

	if (cmdq->item == NULL) {
		cmdq->item = TAILQ_FIRST(&cmdq->queue);
		cmdq->cmd = TAILQ_FIRST(&cmdq->item->cmdlist->list);
	} else
		cmdq->cmd = TAILQ_NEXT(cmdq->cmd, qentry);

	do {
		next = TAILQ_NEXT(cmdq->item, qentry);

		while (cmdq->cmd != NULL) {
			cmd_print(cmdq->cmd, s, sizeof s);
			log_debug("cmdq %p: %s (client %d)", cmdq, s,
			    cmdq->client != NULL ? cmdq->client->ibuf.fd : -1);

			cmdq->time = time(NULL);
			cmdq->number++;

			guard = cmdq_guard(cmdq, "begin");
			retval = cmdq->cmd->entry->exec(cmdq->cmd, cmdq);
			if (guard) {
				if (retval == CMD_RETURN_ERROR)
				    cmdq_guard(cmdq, "error");
				else
				    cmdq_guard(cmdq, "end");
			}

			if (retval == CMD_RETURN_ERROR)
				break;
			if (retval == CMD_RETURN_WAIT)
				goto out;
			if (retval == CMD_RETURN_STOP) {
				cmdq_flush(cmdq);
				goto empty;
			}

			cmdq->cmd = TAILQ_NEXT(cmdq->cmd, qentry);
		}

		TAILQ_REMOVE(&cmdq->queue, cmdq->item, qentry);
		cmd_list_free(cmdq->item->cmdlist);
		free(cmdq->item);

		cmdq->item = next;
		if (cmdq->item != NULL)
			cmdq->cmd = TAILQ_FIRST(&cmdq->item->cmdlist->list);
	} while (cmdq->item != NULL);

empty:
	if (cmdq->client_exit)
		cmdq->client->flags |= CLIENT_EXIT;
	if (cmdq->emptyfn != NULL)
		cmdq->emptyfn(cmdq); /* may free cmdq */
	empty = 1;

out:
	notify_enable();
	return (empty);
}

/* Flush command queue. */
void
cmdq_flush(struct cmd_q *cmdq)
{
	struct cmd_q_item	*item, *item1;

	TAILQ_FOREACH_SAFE(item, &cmdq->queue, qentry, item1) {
		TAILQ_REMOVE(&cmdq->queue, item, qentry);
		cmd_list_free(item->cmdlist);
		free(item);
	}
	cmdq->item = NULL;
}
