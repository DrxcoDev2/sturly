// SPDX-License-Identifier: GPL-2.0
#include "bcachefs.h"
#include "btree_cache.h"
#include "btree_iter.h"
#include "error.h"
#include "fs-common.h"
#include "journal.h"
#include "recovery_passes.h"
#include "super.h"
#include "thread_with_file.h"

#define FSCK_ERR_RATELIMIT_NR	10

void bch2_log_msg_start(struct bch_fs *c, struct printbuf *out)
{
#ifdef BCACHEFS_LOG_PREFIX
	prt_printf(out, bch2_log_msg(c, ""));
#endif
	printbuf_indent_add(out, 2);
}

bool bch2_inconsistent_error(struct bch_fs *c)
{
	set_bit(BCH_FS_error, &c->flags);

	switch (c->opts.errors) {
	case BCH_ON_ERROR_continue:
		return false;
	case BCH_ON_ERROR_fix_safe:
	case BCH_ON_ERROR_ro:
		if (bch2_fs_emergency_read_only(c))
			bch_err(c, "inconsistency detected - emergency read only at journal seq %llu",
				journal_cur_seq(&c->journal));
		return true;
	case BCH_ON_ERROR_panic:
		panic(bch2_fmt(c, "panic after error"));
		return true;
	default:
		BUG();
	}
}

int bch2_topology_error(struct bch_fs *c)
{
	set_bit(BCH_FS_topology_error, &c->flags);
	if (!test_bit(BCH_FS_recovery_running, &c->flags)) {
		bch2_inconsistent_error(c);
		return -BCH_ERR_btree_need_topology_repair;
	} else {
		return bch2_run_explicit_recovery_pass(c, BCH_RECOVERY_PASS_check_topology) ?:
			-BCH_ERR_btree_node_read_validate_error;
	}
}

void bch2_fatal_error(struct bch_fs *c)
{
	if (bch2_fs_emergency_read_only(c))
		bch_err(c, "fatal error - emergency read only");
}

void bch2_io_error_work(struct work_struct *work)
{
	struct bch_dev *ca = container_of(work, struct bch_dev, io_error_work);
	struct bch_fs *c = ca->fs;
	bool dev;

	down_write(&c->state_lock);
	dev = bch2_dev_state_allowed(c, ca, BCH_MEMBER_STATE_ro,
				    BCH_FORCE_IF_DEGRADED);
	if (dev
	    ? __bch2_dev_set_state(c, ca, BCH_MEMBER_STATE_ro,
				  BCH_FORCE_IF_DEGRADED)
	    : bch2_fs_emergency_read_only(c))
		bch_err(ca,
			"too many IO errors, setting %s RO",
			dev ? "device" : "filesystem");
	up_write(&c->state_lock);
}

void bch2_io_error(struct bch_dev *ca, enum bch_member_error_type type)
{
	atomic64_inc(&ca->errors[type]);
	//queue_work(system_long_wq, &ca->io_error_work);
}

enum ask_yn {
	YN_NO,
	YN_YES,
	YN_ALLNO,
	YN_ALLYES,
};

static enum ask_yn parse_yn_response(char *buf)
{
	buf = strim(buf);

	if (strlen(buf) == 1)
		switch (buf[0]) {
		case 'n':
			return YN_NO;
		case 'y':
			return YN_YES;
		case 'N':
			return YN_ALLNO;
		case 'Y':
			return YN_ALLYES;
		}
	return -1;
}

#ifdef __KERNEL__
static enum ask_yn bch2_fsck_ask_yn(struct bch_fs *c, struct btree_trans *trans)
{
	struct stdio_redirect *stdio = c->stdio;

	if (c->stdio_filter && c->stdio_filter != current)
		stdio = NULL;

	if (!stdio)
		return YN_NO;

	if (trans)
		bch2_trans_unlock(trans);

	unsigned long unlock_long_at = trans ? jiffies + HZ * 2 : 0;
	darray_char line = {};
	int ret;

	do {
		unsigned long t;
		bch2_print(c, " (y,n, or Y,N for all errors of this type) ");
rewait:
		t = unlock_long_at
			? max_t(long, unlock_long_at - jiffies, 0)
			: MAX_SCHEDULE_TIMEOUT;

		int r = bch2_stdio_redirect_readline_timeout(stdio, &line, t);
		if (r == -ETIME) {
			bch2_trans_unlock_long(trans);
			unlock_long_at = 0;
			goto rewait;
		}

		if (r < 0) {
			ret = YN_NO;
			break;
		}

		darray_last(line) = '\0';
	} while ((ret = parse_yn_response(line.data)) < 0);

	darray_exit(&line);
	return ret;
}
#else

#include "tools-util.h"

static enum ask_yn bch2_fsck_ask_yn(struct bch_fs *c, struct btree_trans *trans)
{
	char *buf = NULL;
	size_t buflen = 0;
	int ret;

	do {
		fputs(" (y,n, or Y,N for all errors of this type) ", stdout);
		fflush(stdout);

		if (getline(&buf, &buflen, stdin) < 0)
			die("error reading from standard input");
	} while ((ret = parse_yn_response(buf)) < 0);

	free(buf);
	return ret;
}

#endif

static struct fsck_err_state *fsck_err_get(struct bch_fs *c, const char *fmt)
{
	struct fsck_err_state *s;

	if (!test_bit(BCH_FS_fsck_running, &c->flags))
		return NULL;

	list_for_each_entry(s, &c->fsck_error_msgs, list)
		if (s->fmt == fmt) {
			/*
			 * move it to the head of the list: repeated fsck errors
			 * are common
			 */
			list_move(&s->list, &c->fsck_error_msgs);
			return s;
		}

	s = kzalloc(sizeof(*s), GFP_NOFS);
	if (!s) {
		if (!c->fsck_alloc_msgs_err)
			bch_err(c, "kmalloc err, cannot ratelimit fsck errs");
		c->fsck_alloc_msgs_err = true;
		return NULL;
	}

	INIT_LIST_HEAD(&s->list);
	s->fmt = fmt;
	list_add(&s->list, &c->fsck_error_msgs);
	return s;
}

/* s/fix?/fixing/ s/recreate?/recreating/ */
static void prt_actioning(struct printbuf *out, const char *action)
{
	unsigned len = strlen(action);

	BUG_ON(action[len - 1] != '?');
	--len;

	if (action[len - 1] == 'e')
		--len;

	prt_bytes(out, action, len);
	prt_str(out, "ing");
}

static const u8 fsck_flags_extra[] = {
#define x(t, n, flags)		[BCH_FSCK_ERR_##t] = flags,
	BCH_SB_ERRS()
#undef x
};

static int do_fsck_ask_yn(struct bch_fs *c,
			  struct btree_trans *trans,
			  struct printbuf *question,
			  const char *action)
{
	prt_str(question, ", ");
	prt_str(question, action);

	if (bch2_fs_stdio_redirect(c))
		bch2_print(c, "%s", question->buf);
	else
		bch2_print_string_as_lines(KERN_ERR, question->buf);

	int ask = bch2_fsck_ask_yn(c, trans);

	if (trans) {
		int ret = bch2_trans_relock(trans);
		if (ret)
			return ret;
	}

	return ask;
}

int __bch2_fsck_err(struct bch_fs *c,
		  struct btree_trans *trans,
		  enum bch_fsck_flags flags,
		  enum bch_sb_error_id err,
		  const char *fmt, ...)
{
	struct fsck_err_state *s = NULL;
	va_list args;
	bool print = true, suppressing = false, inconsistent = false, exiting = false;
	struct printbuf buf = PRINTBUF, *out = &buf;
	int ret = -BCH_ERR_fsck_ignore;
	const char *action_orig = "fix?", *action = action_orig;

	might_sleep();

	if (!WARN_ON(err >= ARRAY_SIZE(fsck_flags_extra)))
		flags |= fsck_flags_extra[err];

	if (!c)
		c = trans->c;

	/*
	 * Ugly: if there's a transaction in the current task it has to be
	 * passed in to unlock if we prompt for user input.
	 *
	 * But, plumbing a transaction and transaction restarts into
	 * bkey_validate() is problematic.
	 *
	 * So:
	 * - make all bkey errors AUTOFIX, they're simple anyways (we just
	 *   delete the key)
	 * - and we don't need to warn if we're not prompting
	 */
	WARN_ON((flags & FSCK_CAN_FIX) &&
		!(flags & FSCK_AUTOFIX) &&
		!trans &&
		bch2_current_has_btree_trans(c));

	if (test_bit(err, c->sb.errors_silent))
		return flags & FSCK_CAN_FIX
			? -BCH_ERR_fsck_fix
			: -BCH_ERR_fsck_ignore;

	bch2_sb_error_count(c, err);

	va_start(args, fmt);
	prt_vprintf(out, fmt, args);
	va_end(args);

	/* Custom fix/continue/recreate/etc.? */
	if (out->buf[out->pos - 1] == '?') {
		const char *p = strrchr(out->buf, ',');
		if (p) {
			out->pos = p - out->buf;
			action = kstrdup(p + 2, GFP_KERNEL);
			if (!action) {
				ret = -ENOMEM;
				goto err;
			}
		}
	}

	mutex_lock(&c->fsck_error_msgs_lock);
	s = fsck_err_get(c, fmt);
	if (s) {
		/*
		 * We may be called multiple times for the same error on
		 * transaction restart - this memoizes instead of asking the user
		 * multiple times for the same error:
		 */
		if (s->last_msg && !strcmp(buf.buf, s->last_msg)) {
			ret = s->ret;
			goto err_unlock;
		}

		kfree(s->last_msg);
		s->last_msg = kstrdup(buf.buf, GFP_KERNEL);
		if (!s->last_msg) {
			ret = -ENOMEM;
			goto err_unlock;
		}

		if (c->opts.ratelimit_errors &&
		    !(flags & FSCK_NO_RATELIMIT) &&
		    s->nr >= FSCK_ERR_RATELIMIT_NR) {
			if (s->nr == FSCK_ERR_RATELIMIT_NR)
				suppressing = true;
			else
				print = false;
		}

		s->nr++;
	}

#ifdef BCACHEFS_LOG_PREFIX
	if (!strncmp(fmt, "bcachefs:", 9))
		prt_printf(out, bch2_log_msg(c, ""));
#endif

	if ((flags & FSCK_AUTOFIX) &&
	    (c->opts.errors == BCH_ON_ERROR_continue ||
	     c->opts.errors == BCH_ON_ERROR_fix_safe)) {
		prt_str(out, ", ");
		if (flags & FSCK_CAN_FIX) {
			prt_actioning(out, action);
			ret = -BCH_ERR_fsck_fix;
		} else {
			prt_str(out, ", continuing");
			ret = -BCH_ERR_fsck_ignore;
		}

		goto print;
	} else if (!test_bit(BCH_FS_fsck_running, &c->flags)) {
		if (c->opts.errors != BCH_ON_ERROR_continue ||
		    !(flags & (FSCK_CAN_FIX|FSCK_CAN_IGNORE))) {
			prt_str(out, ", shutting down");
			inconsistent = true;
			ret = -BCH_ERR_fsck_errors_not_fixed;
		} else if (flags & FSCK_CAN_FIX) {
			prt_str(out, ", ");
			prt_actioning(out, action);
			ret = -BCH_ERR_fsck_fix;
		} else {
			prt_str(out, ", continuing");
			ret = -BCH_ERR_fsck_ignore;
		}
	} else if (c->opts.fix_errors == FSCK_FIX_exit) {
		prt_str(out, ", exiting");
		ret = -BCH_ERR_fsck_errors_not_fixed;
	} else if (flags & FSCK_CAN_FIX) {
		int fix = s && s->fix
			? s->fix
			: c->opts.fix_errors;

		if (fix == FSCK_FIX_ask) {
			print = false;

			ret = do_fsck_ask_yn(c, trans, out, action);
			if (ret < 0)
				goto err_unlock;

			if (ret >= YN_ALLNO && s)
				s->fix = ret == YN_ALLNO
					? FSCK_FIX_no
					: FSCK_FIX_yes;

			ret = ret & 1
				? -BCH_ERR_fsck_fix
				: -BCH_ERR_fsck_ignore;
		} else if (fix == FSCK_FIX_yes ||
			   (c->opts.nochanges &&
			    !(flags & FSCK_CAN_IGNORE))) {
			prt_str(out, ", ");
			prt_actioning(out, action);
			ret = -BCH_ERR_fsck_fix;
		} else {
			prt_str(out, ", not ");
			prt_actioning(out, action);
		}
	} else if (!(flags & FSCK_CAN_IGNORE)) {
		prt_str(out, " (repair unimplemented)");
	}

	if (ret == -BCH_ERR_fsck_ignore &&
	    (c->opts.fix_errors == FSCK_FIX_exit ||
	     !(flags & FSCK_CAN_IGNORE)))
		ret = -BCH_ERR_fsck_errors_not_fixed;

	if (test_bit(BCH_FS_fsck_running, &c->flags) &&
	    (ret != -BCH_ERR_fsck_fix &&
	     ret != -BCH_ERR_fsck_ignore)) {
		exiting = true;
		print = true;
	}
print:
	if (print) {
		if (bch2_fs_stdio_redirect(c))
			bch2_print(c, "%s\n", out->buf);
		else
			bch2_print_string_as_lines(KERN_ERR, out->buf);
	}

	if (exiting)
		bch_err(c, "Unable to continue, halting");
	else if (suppressing)
		bch_err(c, "Ratelimiting new instances of previous error");

	if (s)
		s->ret = ret;

	if (inconsistent)
		bch2_inconsistent_error(c);

	/*
	 * We don't yet track whether the filesystem currently has errors, for
	 * log_fsck_err()s: that would require us to track for every error type
	 * which recovery pass corrects it, to get the fsck exit status correct:
	 */
	if (flags & FSCK_CAN_FIX) {
		if (ret == -BCH_ERR_fsck_fix) {
			set_bit(BCH_FS_errors_fixed, &c->flags);
		} else {
			set_bit(BCH_FS_errors_not_fixed, &c->flags);
			set_bit(BCH_FS_error, &c->flags);
		}
	}
err_unlock:
	mutex_unlock(&c->fsck_error_msgs_lock);
err:
	if (action != action_orig)
		kfree(action);
	printbuf_exit(&buf);
	return ret;
}

static const char * const bch2_bkey_validate_contexts[] = {
#define x(n) #n,
	BKEY_VALIDATE_CONTEXTS()
#undef x
	NULL
};

int __bch2_bkey_fsck_err(struct bch_fs *c,
			 struct bkey_s_c k,
			 struct bkey_validate_context from,
			 enum bch_sb_error_id err,
			 const char *fmt, ...)
{
	if (from.flags & BCH_VALIDATE_silent)
		return -BCH_ERR_fsck_delete_bkey;

	unsigned fsck_flags = 0;
	if (!(from.flags & (BCH_VALIDATE_write|BCH_VALIDATE_commit))) {
		if (test_bit(err, c->sb.errors_silent))
			return -BCH_ERR_fsck_delete_bkey;

		fsck_flags |= FSCK_AUTOFIX|FSCK_CAN_FIX;
	}
	if (!WARN_ON(err >= ARRAY_SIZE(fsck_flags_extra)))
		fsck_flags |= fsck_flags_extra[err];

	struct printbuf buf = PRINTBUF;
	prt_printf(&buf, "invalid bkey in %s",
		   bch2_bkey_validate_contexts[from.from]);

	if (from.from == BKEY_VALIDATE_journal)
		prt_printf(&buf, " journal seq=%llu offset=%u",
			   from.journal_seq, from.journal_offset);

	prt_str(&buf, " btree=");
	bch2_btree_id_to_text(&buf, from.btree);
	prt_printf(&buf, " level=%u: ", from.level);

	bch2_bkey_val_to_text(&buf, c, k);
	prt_str(&buf, "\n  ");

	va_list args;
	va_start(args, fmt);
	prt_vprintf(&buf, fmt, args);
	va_end(args);

	prt_str(&buf, ": delete?");

	int ret = __bch2_fsck_err(c, NULL, fsck_flags, err, "%s", buf.buf);
	printbuf_exit(&buf);
	return ret;
}

void bch2_flush_fsck_errs(struct bch_fs *c)
{
	struct fsck_err_state *s, *n;

	mutex_lock(&c->fsck_error_msgs_lock);

	list_for_each_entry_safe(s, n, &c->fsck_error_msgs, list) {
		if (s->ratelimited && s->last_msg)
			bch_err(c, "Saw %llu errors like:\n    %s", s->nr, s->last_msg);

		list_del(&s->list);
		kfree(s->last_msg);
		kfree(s);
	}

	mutex_unlock(&c->fsck_error_msgs_lock);
}

int bch2_inum_err_msg_trans(struct btree_trans *trans, struct printbuf *out, subvol_inum inum)
{
	u32 restart_count = trans->restart_count;
	int ret = 0;

	/* XXX: we don't yet attempt to print paths when we don't know the subvol */
	if (inum.subvol)
		ret = lockrestart_do(trans, bch2_inum_to_path(trans, inum, out));
	if (!inum.subvol || ret)
		prt_printf(out, "inum %llu:%llu", inum.subvol, inum.inum);

	return trans_was_restarted(trans, restart_count);
}

int bch2_inum_offset_err_msg_trans(struct btree_trans *trans, struct printbuf *out,
				    subvol_inum inum, u64 offset)
{
	int ret = bch2_inum_err_msg_trans(trans, out, inum);
	prt_printf(out, " offset %llu: ", offset);
	return ret;
}

void bch2_inum_err_msg(struct bch_fs *c, struct printbuf *out, subvol_inum inum)
{
	bch2_trans_run(c, bch2_inum_err_msg_trans(trans, out, inum));
}

void bch2_inum_offset_err_msg(struct bch_fs *c, struct printbuf *out,
			      subvol_inum inum, u64 offset)
{
	bch2_trans_run(c, bch2_inum_offset_err_msg_trans(trans, out, inum, offset));
}
