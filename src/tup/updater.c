#define _ATFILE_SOURCE
#include "updater.h"
#include "graph.h"
#include "fileio.h"
#include "debug.h"
#include "db.h"
#include "entry.h"
#include "parser.h"
#include "server.h"
#include "fslurp.h"
#include "array_size.h"
#include "config.h"
#include "monitor.h"
#include "path.h"
#include "colors.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/time.h>

#define MAX_JOBS 65535

static int run_scan(void);
static int update_tup_config(void);
static int process_create_nodes(void);
static int process_update_nodes(int argc, char **argv, int *num_pruned);
static int check_create_todo(void);
static int check_update_todo(int argc, char **argv);
static int build_graph(struct graph *g);
static int add_file_cb(void *arg, struct tup_entry *tent, int style);
static int execute_graph(struct graph *g, int keep_going, int jobs,
			 void *(*work_func)(void *));

static void *create_work(void *arg);
static void *update_work(void *arg);
static void *todo_work(void *arg);
static int update(struct node *n);
static void tup_show_message(const char *s);
static void tup_main_progress(const char *s);
static void show_progress(int sum, int total, struct tup_entry *tent);

static int do_keep_going;
static int num_jobs;
static int warnings;

static pthread_mutex_t db_mutex;

static const char *signal_err[] = {
	NULL, /* 0 */
	"Hangup detected on controlling terminal or death of controlling process",
	"Interrupt from keyboard",
	"Quit from keyboard",
	"Illegal Instruction",
	NULL, /* 5 */
	"Abort signal from abort(3)",
	NULL,
	"Floating point exception",
	"Kill signal",
	NULL, /* 10 */
	"Segmentation fault",
	NULL,
	"Broken pipe: write to pipe with no readers",
	"Timer signal from alarm(2)",
	"Termination signal", /* 15 */
};

/* The basic gist of these guys is they start out on the free_list, then they
 * move to the active_list once they get setup with 'n' pointing to the node
 * they should process, and their cond variable is signalled. Once they are
 * done processing, they set retn to n and rc to the return code (ie: 0 for
 * success, or an error code) for that node. They then move themselves over to
 * the fin_list and signal the list_cond so that execute_graph() knows this
 * work is done.
 */
struct worker_thread {
	struct list_head list;
	pthread_t pid;
	struct graph *g; /* This should only be used in create_work() and todo_work */

	pthread_mutex_t lock;
	pthread_cond_t cond;

	pthread_mutex_t *list_mutex;
	pthread_cond_t *list_cond;
	struct list_head *fin_list;
	struct node *n;
	struct node *retn;
	int rc;
	int quit;
};

int updater(int argc, char **argv, int phase)
{
	int x;
	int do_scan = 1;
	int num_pruned = 0;
	int rc = -1;

	if(tup_db_config_get_int("keep_going", 0, &do_keep_going) < 0)
		return -1;
	if(tup_db_config_get_int("num_jobs", 1, &num_jobs) < 0)
		return -1;

	argc--;
	argv++;

	for(x=0; x<argc; x++) {
		if(strcmp(argv[x], "-d") == 0) {
			debug_enable("tup.updater");
		} else if(strcmp(argv[x], "--keep-going") == 0 ||
			  strcmp(argv[x], "-k") == 0) {
			do_keep_going = 1;
		} else if(strcmp(argv[x], "--no-keep-going") == 0) {
			do_keep_going = 0;
		} else if(strcmp(argv[x], "--no-scan") == 0) {
			do_scan = 0;
		} else if(strncmp(argv[x], "-j", 2) == 0) {
			num_jobs = strtol(argv[x]+2, NULL, 0);
		} else if(strcmp(argv[x], "--") == 0) {
			break;
		}
	}

	if(num_jobs < 1) {
		fprintf(stderr, "Warning: Setting the number of jobs to 1\n");
		num_jobs = 1;
	}
	if(num_jobs > MAX_JOBS) {
		fprintf(stderr, "Warning: Setting the number of jobs to MAX_JOBS\n");
		num_jobs = MAX_JOBS;
	}

	if(do_scan) {
		if(run_scan() < 0)
			return -1;
	}
	if(update_tup_config() < 0)
		goto out;
	if(phase == 1) { /* Collect underpants */
		rc = 0;
		goto out;
	}
	if(process_create_nodes() < 0)
		goto out;
	if(phase == 2) { /* ? */
		rc = 0;
		goto out;
	}
	if(process_update_nodes(argc, argv, &num_pruned) < 0)
		goto out;
	if(num_pruned) {
		tup_main_progress("Partial update complete:");
		printf(" skipped %i commands.\n", num_pruned);
	} else {
		tup_main_progress("Updated.\n");
	}
	rc = 0;
out:
	if(server_quit() < 0)
		rc = -1;
	return rc; /* Profit! */
}

int todo(int argc, char **argv)
{
	int x;
	int rc;
	int do_scan = 1;

	argc--;
	argv++;

	for(x=0; x<argc; x++) {
		if(strcmp(argv[x], "--no-scan") == 0) {
			do_scan = 0;
		} else if(strcmp(argv[x], "--") == 0) {
			break;
		}
	}

	if(do_scan) {
		if(run_scan() < 0)
			return -1;
	}

	rc = tup_db_in_create_list(VAR_DT);
	if(rc < 0)
		return -1;
	if(rc == 1) {
		printf("The tup.config file has been modified and needs to be read.\n");
		printf("Run 'tup read' to proceed to phase 1.\n");
		return 0;
	}

	rc = check_create_todo();
	if(rc < 0)
		return -1;
	if(rc == 1) {
		printf("Run 'tup parse' to proceed to phase 2.\n");
		return 0;
	}

	rc = check_update_todo(argc, argv);
	if(rc < 0)
		return -1;
	if(rc == 1) {
		printf("Run 'tup upd' to bring the system up-to-date.\n");
		return 0;
	}
	printf("tup: Everything is up-to-date.\n");
	return 0;
}

static int run_scan(void)
{
	int rc;

	rc = monitor_get_pid(0);
	if(rc < 0) {
		fprintf(stderr, "tup error: Unable to determine if the file monitor is still running.\n");
		return -1;
	}
	if(rc == 0) {
		struct timeval t1, t2;
		tup_main_progress("Scanning filesystem...");
		fflush(stdout);
		gettimeofday(&t1, NULL);
		if(tup_scan() < 0)
			return -1;
		gettimeofday(&t2, NULL);
		printf("%.3fs\n",
		       (double)(t2.tv_sec - t1.tv_sec) +
		       (double)(t2.tv_usec - t1.tv_usec)/1e6);
	} else {
		tup_main_progress("No filesystem scan - monitor is running.\n");
	}
	return 0;
}

static int delete_files(struct graph *g)
{
	struct rb_node *rbn;
	int num_deleted = 0;
	struct tup_entry *tent;
	struct list_head *entrylist;
	int rc = -1;

	if(g->delete_count) {
		tup_main_progress("Deleting files...\n");
	} else {
		tup_main_progress("No files to delete.\n");
	}
	entrylist = tup_entry_get_list();
	while((rbn = rb_first(&g->delete_tree)) != NULL) {
		struct tupid_tree *tt = rb_entry(rbn, struct tupid_tree, rbn);
		struct tree_entry *te = container_of(tt, struct tree_entry, tnode);
		int do_delete;

		do_delete = 1;
		if(te->type == TUP_NODE_GENERATED) {
			int tmp;

			if(tup_entry_add(tt->tupid, &tent) < 0)
				goto out_err;

			tmp = tup_db_in_modify_list(tt->tupid);
			if(tmp < 0)
				goto out_err;
			if(tmp == 1) {
				tup_entry_list_add(tent, entrylist);
				do_delete = 0;
			}

			/* Only delete if the file wasn't modified (t6031) */
			if(do_delete) {
				show_progress(num_deleted, g->delete_count, tent);
				num_deleted++;
				if(delete_file(tent->dt, tent->name.s) < 0)
					goto out_err;
			}
		}
		if(do_delete) {
			if(tup_del_id_force(te->tnode.tupid, te->type) < 0)
				goto out_err;
		}
		rb_erase(rbn, &g->delete_tree);
		free(te);
	}
	if(!list_empty(entrylist)) {
		tup_show_message("Converting generated files to normal files...\n");
	}
	list_for_each_entry(tent, entrylist, list) {
		if(tup_db_set_type(tent, TUP_NODE_FILE) < 0)
			goto out_err;
		show_progress(num_deleted, g->delete_count, tent);
		num_deleted++;
	}
	if(g->delete_count) {
		show_progress(g->delete_count, g->delete_count, NULL);
	}
	rc = 0;
out_err:
	tup_entry_release_list();
	return rc;
}

static int update_tup_config(void)
{
	int rc;

	rc = tup_db_in_create_list(VAR_DT);
	if(rc < 0)
		return -1;
	if(rc == 1) {
		if(tup_db_begin() < 0)
			return -1;
		if(tup_db_unflag_create(VAR_DT) < 0)
			return -1;
		tup_main_progress("Reading in new configuration variables...\n");
		rc = tup_db_read_vars(DOT_DT, TUP_CONFIG);
		if(rc == 0) {
			tup_db_commit();
		} else {
			tup_db_rollback();
			return -1;
		}
	} else {
		tup_main_progress("No tup.config changes.\n");
	}
	if(tup_vardict_open() < 0)
		return -1;

	return 0;
}

static int process_create_nodes(void)
{
	struct graph g;
	int rc;

	if(create_graph(&g, TUP_NODE_DIR) < 0)
		return -1;
	if(tup_db_select_node_by_flags(add_file_cb, &g, TUP_FLAGS_CREATE) < 0)
		return -1;
	if(build_graph(&g) < 0)
		return -1;
	if(g.num_nodes) {
		tup_main_progress("Parsing Tupfiles...\n");
	} else {
		tup_main_progress("No Tupfiles to parse.\n");
	}
	if(graph_empty(&g)) {
		tup_main_progress("No files to delete.\n");
		goto out_destroy;
	}

	tup_db_begin();
	if(server_init(SERVER_PARSER_MODE, &g.delete_tree) < 0) {
		return -1;
	}
	/* create_work must always use only 1 thread since no locking is done */
	compat_lock_disable();
	rc = execute_graph(&g, 0, 1, create_work);
	compat_lock_enable();
	if(rc == 0)
		rc = delete_files(&g);
	if(rc == 0) {
		tup_db_commit();
	} else if(rc == -1) {
		tup_db_rollback();
		return -1;
	} else {
		fprintf(stderr, "tup error: execute_graph returned %i - abort. This is probably a bug.\n", rc);
		return -1;
	}

out_destroy:
	if(destroy_graph(&g) < 0)
		return -1;
	return 0;
}

static int process_update_nodes(int argc, char **argv, int *num_pruned)
{
	struct graph g;
	int rc;

	if(create_graph(&g, TUP_NODE_CMD) < 0)
		return -1;
	if(tup_db_select_node_by_flags(add_file_cb, &g, TUP_FLAGS_MODIFY) < 0)
		return -1;
	if(build_graph(&g) < 0)
		return -1;

	if(prune_graph(&g, argc, argv, num_pruned) < 0)
		return -1;

	if(g.num_nodes) {
		tup_main_progress("Executing Commands...\n");
	} else {
		tup_main_progress("No commands to execute.\n");
	}
	/* If the graph only has the root node, just bail. Note that this
	 * may be different from g.num_nodes==0, since that only counts
	 * command nodes (we may have to go through the DAG and clear out
	 * some files marked modify that don't actually point to commands).
	 */
	if(graph_empty(&g))
		goto out_destroy;

	if(pthread_mutex_init(&db_mutex, NULL) != 0) {
		perror("pthread_mutex_init");
		return -1;
	}
	tup_db_begin();
	warnings = 0;
	if(server_init(SERVER_UPDATER_MODE, NULL) < 0) {
		return -1;
	}
	rc = execute_graph(&g, do_keep_going, num_jobs, update_work);
	if(warnings) {
		fprintf(stderr, "tup warning: Update resulted in %i warning%s\n", warnings, warnings == 1 ? "" : "s");
	}
	if(rc == -2) {
		fprintf(stderr, "tup error: execute_graph returned %i - abort. This is probably a bug.\n", rc);
		return -1;
	}
	tup_db_commit();
	pthread_mutex_destroy(&db_mutex);
	if(rc < 0)
		return -1;
out_destroy:
	if(destroy_graph(&g) < 0)
		return -1;
	return 0;
}

static int check_create_todo(void)
{
	struct graph g;
	int rc;
	int stuff_todo = 0;

	if(create_graph(&g, TUP_NODE_DIR) < 0)
		return -1;
	if(tup_db_select_node_by_flags(add_file_cb, &g, TUP_FLAGS_CREATE) < 0)
		return -1;
	if(build_graph(&g) < 0)
		return -1;
	if(g.num_nodes) {
		printf("Tup phase 1: The following directories must be parsed:\n");
		stuff_todo = 1;
	}
	rc = execute_graph(&g, 0, 1, todo_work);
	if(rc == 0) {
		rc = stuff_todo;
	} else if(rc == -1) {
		return -1;
	} else {
		fprintf(stderr, "tup error: execute_graph returned %i - abort. This is probably a bug.\n", rc);
		return -1;
	}
	if(destroy_graph(&g) < 0)
		return -1;
	return rc;
}

static int check_update_todo(int argc, char **argv)
{
	struct graph g;
	int rc;
	int stuff_todo = 0;
	int num_pruned = 0;

	if(create_graph(&g, TUP_NODE_CMD) < 0)
		return -1;
	if(tup_db_select_node_by_flags(add_file_cb, &g, TUP_FLAGS_MODIFY) < 0)
		return -1;
	if(build_graph(&g) < 0)
		return -1;
	if(prune_graph(&g, argc, argv, &num_pruned) < 0)
		return -1;
	if(g.num_nodes) {
		printf("Tup phase 3: The following %i command%s will be executed:\n", g.num_nodes, g.num_nodes == 1 ? "" : "s");
		stuff_todo = 1;
	}
	rc = execute_graph(&g, 0, 1, todo_work);
	if(rc == 0) {
		rc = stuff_todo;
	} else if(rc == -1) {
		return -1;
	} else {
		fprintf(stderr, "tup error: execute_graph returned %i - abort. This is probably a bug.\n", rc);
		return -1;
	}
	if(num_pruned) {
		printf("Partial update: %i command%s will be skipped.\n", num_pruned, num_pruned == 1 ? "" : "s");
	}
	if(destroy_graph(&g) < 0)
		return -1;
	return rc;
}

static int build_graph(struct graph *g)
{
	struct node *cur;

	while(!list_empty(&g->plist)) {
		cur = list_entry(g->plist.next, struct node, list);
		if(cur->state == STATE_INITIALIZED) {
			DEBUGP("find deps for node: %lli\n", cur->tnode.tupid);
			g->cur = cur;
			if(tup_db_select_node_by_link(add_file_cb, g, cur->tnode.tupid) < 0)
				return -1;
			cur->state = STATE_PROCESSING;
		} else if(cur->state == STATE_PROCESSING) {
			DEBUGP("remove node from stack: %lli\n", cur->tnode.tupid);
			list_del(&cur->list);
			list_add_tail(&cur->list, &g->node_list);
			cur->state = STATE_FINISHED;
		} else if(cur->state == STATE_FINISHED) {
			fprintf(stderr, "tup internal error: STATE_FINISHED node %lli in plist\n", cur->tnode.tupid);
			tup_db_print(stderr, cur->tnode.tupid);
			return -1;
		}
	}

	return 0;
}

static int add_file_cb(void *arg, struct tup_entry *tent, int style)
{
	struct graph *g = arg;
	struct node *n;

	n = find_node(g, tent->tnode.tupid);
	if(n != NULL)
		goto edge_create;
	n = create_node(g, tent);
	if(!n)
		return -1;

edge_create:
	if(n->state == STATE_PROCESSING) {
		/* A circular dependency is not guaranteed to trigger this,
		 * but it is easy to check before going through the graph.
		 */
		fprintf(stderr, "Error: Circular dependency detected! "
			"Last edge was: %lli -> %lli\n",
			g->cur->tnode.tupid, tent->tnode.tupid);
		return -1;
	}
	if(style & TUP_LINK_NORMAL && n->expanded == 0) {
		if(n->tent->type == g->count_flags)
			g->num_nodes++;
		n->expanded = 1;
		list_move(&n->list, &g->plist);
	}

	if(create_edge(g->cur, n, style) < 0)
		return -1;
	return 0;
}

static void pop_node(struct graph *g, struct node *n)
{
	while(!list_empty(&n->edges)) {
		struct edge *e;
		e = list_entry(n->edges.next, struct edge, list);
		if(e->dest->state != STATE_PROCESSING) {
			/* Put the node back on the plist, and mark it as such
			 * by changing the state to STATE_PROCESSING.
			 */
			list_move(&e->dest->list, &g->plist);
			e->dest->state = STATE_PROCESSING;
		}

		remove_edge(e);
	}
}

/* Returns:
 *   0: everything built ok
 *  -1: a command failed
 *  -2: a system call failed (some work threads may still be active)
 */
static int execute_graph(struct graph *g, int keep_going, int jobs,
			 void *(*work_func)(void *))
{
	struct node *root;
	struct worker_thread *workers;
	int num_processed = 0;
	int rc = -1;
	int x;
	int active = 0;
	pthread_mutex_t list_mutex;
	pthread_cond_t list_cond;
	LIST_HEAD(active_list);
	LIST_HEAD(fin_list);
	LIST_HEAD(free_list);

	if(pthread_mutex_init(&list_mutex, NULL) != 0) {
		perror("pthread_mutex_init");
		return -2;
	}
	if(pthread_cond_init(&list_cond, NULL) != 0) {
		perror("pthread_cond_init");
		return -2;
	}

	workers = malloc(sizeof(*workers) * jobs);
	if(!workers) {
		perror("malloc");
		return -2;
	}
	for(x=0; x<jobs; x++) {
		workers[x].g = g;

		if(pthread_mutex_init(&workers[x].lock, NULL) != 0) {
			perror("pthread_mutex_init");
			return -2;
		}
		if(pthread_cond_init(&workers[x].cond, NULL) != 0) {
			perror("pthread_cond_init");
			return -2;
		}

		workers[x].list_mutex = &list_mutex;
		workers[x].list_cond = &list_cond;
		workers[x].fin_list = &fin_list;
		workers[x].n = NULL;
		workers[x].retn = NULL;
		workers[x].rc = -1;
		workers[x].quit = 0;
		list_add(&workers[x].list, &free_list);

		if(pthread_create(&workers[x].pid, NULL, work_func, &workers[x]) < 0) {
			perror("pthread_create");
			return -2;
		}
	}

	root = list_entry(g->node_list.next, struct node, list);
	DEBUGP("root node: %lli\n", root->tnode.tupid);
	list_del(&root->list);
	pop_node(g, root);
	remove_node(g, root);

	while(!list_empty(&g->plist) && !server_is_dead()) {
		struct node *n;
		struct worker_thread *wt;
		n = list_entry(g->plist.next, struct node, list);
		DEBUGP("cur node: %lli\n", n->tnode.tupid);
		if(!list_empty(&n->incoming)) {
			/* Here STATE_FINISHED means we're on the node_list,
			 * therefore not ready for processing.
			 */
			list_move(&n->list, &g->node_list);
			n->state = STATE_FINISHED;
			goto check_empties;
		}

		if(!n->expanded) {
			list_del(&n->list);
			pop_node(g, n);
			remove_node(g, n);
			goto check_empties;
		}

		if(n->tent->type == g->count_flags) {
			show_progress(num_processed, g->num_nodes, n->tent);
			num_processed++;
		}
		list_del(&n->list);
		active++;

		wt = list_entry(free_list.next, struct worker_thread, list);
		pthread_mutex_lock(&list_mutex);
		list_move(&wt->list, &active_list);
		pthread_mutex_unlock(&list_mutex);

		pthread_mutex_lock(&wt->lock);
		wt->n = n;
		wt->rc = -1;
		pthread_cond_signal(&wt->cond);
		pthread_mutex_unlock(&wt->lock);

check_empties:
		/* Keep looking for dudes to return as long as:
		 *  1) There are no more free workers
		 *  2) There is no work to do (plist is empty or the server is
		 *     dead) and some people are active.
		 */
		while(list_empty(&free_list) ||
		      ((list_empty(&g->plist) || server_is_dead()) && active)) {
			pthread_mutex_lock(&list_mutex);
			while(list_empty(&fin_list)) {
				pthread_cond_wait(&list_cond, &list_mutex);
			}
			wt = list_entry(fin_list.next, struct worker_thread, list);
			n = wt->retn;
			wt->retn = NULL;
			list_move(&wt->list, &free_list);
			pthread_mutex_unlock(&list_mutex);
			active--;

			if(wt->rc < 0) {
				if(keep_going)
					goto keep_going;
				goto out;
			}
			pop_node(g, n);

keep_going:
			remove_node(g, n);
		}
	}
	if(!list_empty(&g->node_list) || !list_empty(&g->plist)) {
		printf("\n");
		if(keep_going) {
			fprintf(stderr, "Remaining nodes skipped due to errors in command execution.\n");
		} else {
			if(server_is_dead()) {
				fprintf(stderr, "Remaining nodes skipped due to caught signal.\n");
			} else {
				struct node *n;
				fprintf(stderr, "fatal tup error: Graph is not empty after execution. This likely indicates a circular dependency.\n");
				fprintf(stderr, "Node list:\n");
				list_for_each_entry(n, &g->node_list, list) {
					fprintf(stderr, " Node[%lli]: %s\n", n->tnode.tupid, n->tent->name.s);
				}
				fprintf(stderr, "plist:\n");
				list_for_each_entry(n, &g->plist, list) {
					fprintf(stderr, " Plist[%lli]: %s\n", n->tnode.tupid, n->tent->name.s);
				}
			}
		}
		goto out;
	}
	show_progress(num_processed, g->num_nodes, NULL);
	rc = 0;
out:
	/* First tell all the threads to quit */
	for(x=0; x<jobs; x++) {
		pthread_mutex_lock(&workers[x].lock);
		workers[x].quit = 1;
		pthread_cond_signal(&workers[x].cond);
		pthread_mutex_unlock(&workers[x].lock);
	}
	/* Then wait for all the threads to quit */
	for(x=0; x<jobs; x++) {
		pthread_join(workers[x].pid, NULL);
		pthread_cond_destroy(&workers[x].cond);
		pthread_mutex_destroy(&workers[x].lock);
	}

	free(workers); /* Viva la revolucion! */
	pthread_mutex_destroy(&list_mutex);
	pthread_cond_destroy(&list_cond);
	return rc;
}

static struct node *worker_wait(struct worker_thread *wt)
{
	pthread_mutex_lock(&wt->lock);
	while(wt->n == NULL && wt->quit == 0) {
		pthread_cond_wait(&wt->cond, &wt->lock);
	}
	pthread_mutex_unlock(&wt->lock);
	if(wt->quit)
		return (void*)-1;
	return wt->n;
}

static void worker_ret(struct worker_thread *wt, int rc)
{
	wt->rc = rc;
	wt->retn = wt->n;
	wt->n = NULL;

	pthread_mutex_lock(wt->list_mutex);
	list_move(&wt->list, wt->fin_list);
	pthread_cond_signal(wt->list_cond);
	pthread_mutex_unlock(wt->list_mutex);
}

static void *create_work(void *arg)
{
	struct worker_thread *wt = arg;
	struct graph *g = wt->g;
	struct node *n;

	while(1) {
		int rc = 0;

		n = worker_wait(wt);
		if(n == (void*)-1)
			break;

		if(n->tent->type == TUP_NODE_DIR) {
			if(n->already_used) {
				printf("Already parsed[%lli]: '%s'\n", n->tnode.tupid, n->tent->name.s);
				rc = 0;
			} else {
				rc = parse(n, g);
			}
		} else if(n->tent->type == TUP_NODE_VAR ||
			  n->tent->type == TUP_NODE_FILE ||
			  n->tent->type == TUP_NODE_GENERATED ||
			  n->tent->type == TUP_NODE_CMD) {
			rc = 0;
		} else {
			fprintf(stderr, "Error: Unknown node type %i with ID %lli named '%s' in create graph.\n", n->tent->type, n->tnode.tupid, n->tent->name.s);
			rc = -1;
		}
		if(tup_db_unflag_create(n->tnode.tupid) < 0)
			rc = -1;

		worker_ret(wt, rc);
	}
	return NULL;
}

static void *update_work(void *arg)
{
	struct worker_thread *wt = arg;
	struct node *n;

	while(1) {
		struct edge *e;
		int rc = 0;

		n = worker_wait(wt);
		if(n == (void*)-1)
			break;

		if(n->tent->type == TUP_NODE_CMD) {
			rc = update(n);

			/* If the command succeeds, mark any next commands (ie:
			 * our output files' output links) as modify in case we
			 * hit an error. Note we don't just mark the output
			 * file as modify since we aren't actually changing the
			 * file. Doing so would muddy the semantics of the
			 * modify list, which is needed in order to convert
			 * generated files to normal files (t6035).
			 */
			if(rc == 0) {
				pthread_mutex_lock(&db_mutex);
				list_for_each_entry(e, &n->edges, list) {
					struct edge *f;

					list_for_each_entry(f, &e->dest->edges, list) {
						if(f->style & TUP_LINK_NORMAL) {
							if(tup_db_add_modify_list(f->dest->tnode.tupid) < 0)
								rc = -1;
						}
					}
				}
				if(tup_db_unflag_modify(n->tnode.tupid) < 0)
					rc = -1;
				pthread_mutex_unlock(&db_mutex);
			}
		} else {
			pthread_mutex_lock(&db_mutex);
			/* Mark the next nodes as modify in case we hit
			 * an error - we'll need to pick up there (t6006).
			 */
			list_for_each_entry(e, &n->edges, list) {
				if(e->style & TUP_LINK_NORMAL) {
					if(tup_db_add_modify_list(e->dest->tnode.tupid) < 0)
						rc = -1;
				}
			}
			if(tup_db_unflag_modify(n->tnode.tupid) < 0)
				rc = -1;
			pthread_mutex_unlock(&db_mutex);
		}

		worker_ret(wt, rc);
	}
	return NULL;
}

static void *todo_work(void *arg)
{
	struct worker_thread *wt = arg;
	struct graph *g = wt->g;
	struct node *n;

	while(1) {
		n = worker_wait(wt);
		if(n == (void*)-1)
			break;

		if(n->tent->type == g->count_flags)
			tup_db_print(stdout, n->tnode.tupid);

		worker_ret(wt, 0);
	}
	return NULL;
}

static int unlink_outputs(int dfd, struct node *n)
{
	struct edge *e;
	struct node *output;
	list_for_each_entry(e, &n->edges, list) {
		output = e->dest;
		if(unlinkat(dfd, output->tent->name.s, 0) < 0) {
			if(errno != ENOENT) {
				perror("unlinkat");
				fprintf(stderr, "tup error: Unable to unlink previous output file: %s\n", output->tent->name.s);
				return -1;
			}
		}
	}
	return 0;
}

static int process_output(struct server *s, tupid_t tupid, const char *name)
{
	if(display_output(s->output_fd, 0, name, num_jobs > 1) < 0)
		return -1;
	if(display_output(s->error_fd, 1, name, num_jobs > 1) < 0)
		return -1;
	if(s->exited) {
		if(s->exit_status == 0) {
			if(write_files(tupid, name, &s->finfo, &warnings, 0) < 0) {
				fprintf(stderr, " *** Command %lli ran successfully, but tup failed to save the dependencies: %s\n", tupid, name);
				return -1;
			}

			/* Hooray! */
			return 0;
		} else {
			fprintf(stderr, " *** Command %lli failed with return value %i: %s\n", tupid, s->exit_status, name);
			if(write_files(tupid, name, &s->finfo, &warnings, 1) < 0) {
				fprintf(stderr, " *** Additionally, command %lli failed to process input dependencies. These should probably be fixed before addressing the command failure.\n", tupid);
			}
			return -1;
		}
	} else if(s->signalled) {
		int sig = s->exit_sig;
		const char *errmsg = "Unknown signal";

		if(sig >= 0 && sig < ARRAY_SIZE(signal_err) && signal_err[sig])
			errmsg = signal_err[sig];
		fprintf(stderr, " *** Command %lli killed by signal %i (%s): %s\n", tupid, sig, errmsg, name);
		return -1;
	} else {
		fprintf(stderr, "tup internal error: Expected s->exited or s->signalled to be set for command %lli: %s", tupid, name);
		return -1;
	}
}

static int update(struct node *n)
{
	int dfd = -1;
	const char *name = n->tent->name.s;
	struct server s;
	int rc;

	if(name[0] == '^') {
		name++;
		while(*name && *name != ' ') {
			/* This space reserved for flags for something. I dunno
			 * what yet.
			 */
			fprintf(stderr, "Error: Unknown ^ flag: '%c'\n", *name);
			name++;
			return -1;
		}
		while(*name && *name != '^') name++;
		if(!*name) {
			fprintf(stderr, "Error: Missing ending '^' flag in command %lli: %s\n", n->tnode.tupid, n->tent->name.s);
			return -1;
		}
		name++;
		while(isspace(*name)) name++;
	}

	dfd = tup_entry_open(n->tent->parent);
	if(dfd < 0) {
		fprintf(stderr, "Error: Unable to open directory for update work.\n");
		tup_db_print(stderr, n->tent->parent->tnode.tupid);
		goto err_out;
	}

	if(unlink_outputs(dfd, n) < 0)
		goto err_close_dfd;

	s.id = n->tnode.tupid;
	s.exited = 0;
	s.signalled = 0;
	s.exit_status = -1;
	s.exit_sig = -1;
	s.output_fd = -1;
	s.error_fd = -1;
	init_file_info(&s.finfo);
	if(server_exec(&s, dfd, name, n->tent->parent) < 0) {
		fprintf(stderr, " *** Command %lli failed: %s\n", n->tnode.tupid, name);
		goto err_close_dfd;
	}
	close(dfd);

	pthread_mutex_lock(&db_mutex);
	rc = process_output(&s, n->tnode.tupid, name);
	pthread_mutex_unlock(&db_mutex);
	return rc;

err_close_dfd:
	close(dfd);
err_out:
	return -1;
}

static int cur_phase = -1;
static void tup_show_message(const char *s)
{
	const char *tup = " tup ";
	printf("[%s%.*s%s%.*s] %s", color_reverse(), cur_phase, tup, color_end(), 5-cur_phase, tup+cur_phase, s);
}

static void tup_main_progress(const char *s)
{
	cur_phase++;
	tup_show_message(s);
}

static void show_progress(int sum, int total, struct tup_entry *tent)
{
	if(total) {
		const int max = 11;
		int fill;
		char buf[12];

		/* If it's a good enough limit for Final Fantasy VII, it's good
		 * enough for me.
		 */
		if(total > 9999) {
			snprintf(buf, sizeof(buf), "   %3i%%     ", sum*100/total);
		} else {
			snprintf(buf, sizeof(buf), " %4i/%-4i ", sum, total);
		}
		fill = max * sum / total;

		if(tent) {
			printf("[%s%s%.*s%s%.*s] ", color_type(tent->type), color_append_reverse(), fill, buf, color_end(), max-fill, buf+fill);
			if(tent) {
				print_tup_entry(stdout, tent);
			}
			printf("\n");
		} else {
			printf("[%s%.*s%s]\n", color_final(), (int)sizeof(buf), buf, color_end());
		}
	}
}
