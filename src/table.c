/*
 * vim:ts=8:expandtab
 *
 * i3 - an improved dynamic tiling window manager
 *
 * © 2009 Michael Stapelberg and contributors
 *
 * See file LICENSE for license information.
 *
 * table.c: Functions/macros for easy modifying/accessing of _the_ table (defining our
 *          layout).
 *
 */
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdbool.h>
#include <assert.h>

#include "data.h"
#include "table.h"
#include "util.h"
#include "i3.h"

int current_workspace = 0;
Workspace workspaces[10];
/* Convenience pointer to the current workspace */
Workspace *c_ws = &workspaces[0];
int current_col = 0;
int current_row = 0;

/*
 * Initialize table
 *
 */
void init_table() {
        memset(workspaces, 0, sizeof(workspaces));

        for (int i = 0; i < 10; i++) {
                workspaces[i].screen = NULL;
                workspaces[i].num = i;
                expand_table_cols(&(workspaces[i]));
                expand_table_rows(&(workspaces[i]));
        }
}

static void new_container(Workspace *workspace, Container **container, int col, int row) {
        Container *new;
        new = *container = calloc(sizeof(Container), 1);
        CIRCLEQ_INIT(&(new->clients));
        new->colspan = 1;
        new->rowspan = 1;
        new->col = col;
        new->row = row;
        new->workspace = workspace;
}

/*
 * Add one row to the table
 *
 */
void expand_table_rows(Workspace *workspace) {
        workspace->rows++;

        for (int c = 0; c < workspace->cols; c++) {
                workspace->table[c] = realloc(workspace->table[c], sizeof(Container*) * workspace->rows);
                new_container(workspace, &(workspace->table[c][workspace->rows-1]), c, workspace->rows-1);
        }
}

/*
 * Adds one row at the head of the table
 *
 */
void expand_table_rows_at_head(Workspace *workspace) {
        workspace->rows++;

        for (int cols = 0; cols < workspace->cols; cols++)
                workspace->table[cols] = realloc(workspace->table[cols], sizeof(Container*) * workspace->rows);

        /* Move the other rows */
        for (int cols = 0; cols < workspace->cols; cols++)
                for (int rows = workspace->rows - 1; rows > 0; rows--) {
                        LOG("Moving row %d to %d\n", rows-1, rows);
                        workspace->table[cols][rows] = workspace->table[cols][rows-1];
                        workspace->table[cols][rows]->row = rows;
                }
        for (int cols = 0; cols < workspace->cols; cols++)
                new_container(workspace, &(workspace->table[cols][0]), cols, 0);
}

/*
 * Add one column to the table
 *
 */
void expand_table_cols(Workspace *workspace) {
        workspace->cols++;

        workspace->table = realloc(workspace->table, sizeof(Container**) * workspace->cols);
        workspace->table[workspace->cols-1] = calloc(sizeof(Container*) * workspace->rows, 1);
        for (int c = 0; c < workspace->rows; c++)
                new_container(workspace, &(workspace->table[workspace->cols-1][c]), workspace->cols-1, c);
}

/*
 * Inserts one column at the table’s head
 *
 */
void expand_table_cols_at_head(Workspace *workspace) {
        workspace->cols++;

        workspace->table = realloc(workspace->table, sizeof(Container**) * workspace->cols);
        workspace->table[workspace->cols-1] = calloc(sizeof(Container*) * workspace->rows, 1);

        /* Move the other columns */
        for (int rows = 0; rows < workspace->rows; rows++)
                for (int cols = workspace->cols - 1; cols > 0; cols--) {
                        LOG("Moving col %d to %d\n", cols-1, cols);
                        workspace->table[cols][rows] = workspace->table[cols-1][rows];
                        workspace->table[cols][rows]->col = cols;
                }

        for (int rows = 0; rows < workspace->rows; rows++)
                new_container(workspace, &(workspace->table[0][rows]), 0, rows);
}

/*
 * Shrinks the table by one column.
 *
 * The containers themselves are freed in move_columns_from() or move_rows_from(). Therefore, this
 * function may only be called from move_*() or after making sure that the containers are freed
 * properly.
 *
 */
static void shrink_table_cols(Workspace *workspace) {
        workspace->cols--;

        /* Free the container-pointers */
        free(workspace->table[workspace->cols]);

        /* Re-allocate the table */
        workspace->table = realloc(workspace->table, sizeof(Container**) * workspace->cols);
}

/*
 * See shrink_table_cols()
 *
 */
static void shrink_table_rows(Workspace *workspace) {
        workspace->rows--;
        for (int cols = 0; cols < workspace->cols; cols++)
                workspace->table[cols] = realloc(workspace->table[cols], sizeof(Container*) * workspace->rows);
}


/*
 * Performs simple bounds checking for the given column/row
 *
 */
bool cell_exists(int col, int row) {
        return (col >= 0 && col < c_ws->cols) &&
               (row >= 0 && row < c_ws->rows);
}

static void free_container(xcb_connection_t *conn, Workspace *workspace, int col, int row) {
        Container *old_container = workspace->table[col][row];

        if (old_container->mode == MODE_STACK)
                leave_stack_mode(conn, old_container);

        /* We need to distribute the space which will now be freed to other containers */
        if (old_container->width_factor > 0) {
                Container *dest_container = NULL;
                /* Check if we got a container to the left… */
                if (col > 0)
                        dest_container = workspace->table[col-1][row];
                /* …or to the right */
                else if ((col+1) < workspace->cols)
                        dest_container = workspace->table[col+1][row];

                if (dest_container != NULL) {
                        if (dest_container->width_factor == 0)
                                dest_container->width_factor = ((float)workspace->rect.width / workspace->cols) / workspace->rect.width;
                        LOG("dest_container->width_factor = %f\n", dest_container->width_factor);
                        dest_container->width_factor += old_container->width_factor;
                        LOG("afterwards it's %f\n", dest_container->width_factor);
                }
        }

        free(old_container);
}

static void move_columns_from(xcb_connection_t *conn, Workspace *workspace, int cols) {
        LOG("firstly freeing \n");

        /* Free the columns which are cleaned up */
        for (int rows = 0; rows < workspace->rows; rows++)
                free_container(conn, workspace, cols-1, rows);

        for (; cols < workspace->cols; cols++)
                for (int rows = 0; rows < workspace->rows; rows++) {
                        LOG("at col = %d, row = %d\n", cols, rows);
                        Container *new_container = workspace->table[cols][rows];

                        LOG("moving cols = %d to cols -1 = %d\n", cols, cols-1);
                        workspace->table[cols-1][rows] = new_container;

                        new_container->row = rows;
                        new_container->col = cols-1;
                }
}

static void move_rows_from(xcb_connection_t *conn, Workspace *workspace, int rows) {
        for (int cols = 0; cols < workspace->cols; cols++)
                free_container(conn, workspace, cols, rows-1);

        for (; rows < workspace->rows; rows++)
                for (int cols = 0; cols < workspace->cols; cols++) {
                        Container *new_container = workspace->table[cols][rows];

                        LOG("moving rows = %d to rows -1 = %d\n", rows, rows - 1);
                        workspace->table[cols][rows-1] = new_container;

                        new_container->row = rows-1;
                        new_container->col = cols;
                }
}

/*
 * Prints the table’s contents in human-readable form for debugging
 *
 */
void dump_table(xcb_connection_t *conn, Workspace *workspace) {
        LOG("dump_table()\n");
        FOR_TABLE(workspace) {
                Container *con = workspace->table[cols][rows];
                LOG("----\n");
                LOG("at col=%d, row=%d\n", cols, rows);
                LOG("currently_focused = %p\n", con->currently_focused);
                Client *loop;
                CIRCLEQ_FOREACH(loop, &(con->clients), clients) {
                        LOG("got client %08x / %s\n", loop->child, loop->name);
                }
                LOG("----\n");
        }
        LOG("done\n");
}

/*
 * Shrinks the table by "compacting" it, that is, removing completely empty rows/columns
 *
 */
void cleanup_table(xcb_connection_t *conn, Workspace *workspace) {
        LOG("cleanup_table()\n");

        /* Check for empty columns if we got more than one column */
        for (int cols = 0; (workspace->cols > 1) && (cols < workspace->cols);) {
                bool completely_empty = true;
                for (int rows = 0; rows < workspace->rows; rows++)
                        if (workspace->table[cols][rows]->currently_focused != NULL) {
                                completely_empty = false;
                                break;
                        }
                if (completely_empty) {
                        LOG("Removing completely empty column %d\n", cols);
                        if (cols < (workspace->cols - 1))
                                move_columns_from(conn, workspace, cols+1);
                        else {
                                for (int rows = 0; rows < workspace->rows; rows++)
                                        free_container(conn, workspace, cols, rows);
                        }
                        shrink_table_cols(workspace);

                        if (workspace->current_col >= workspace->cols)
                                workspace->current_col = workspace->cols - 1;
                } else cols++;
        }

        /* Check for empty rows if we got more than one row */
        for (int rows = 0; (workspace->rows > 1) && (rows < workspace->rows);) {
                bool completely_empty = true;
                LOG("Checking row %d\n", rows);
                for (int cols = 0; cols < workspace->cols; cols++)
                        if (workspace->table[cols][rows]->currently_focused != NULL) {
                                completely_empty = false;
                                break;
                        }
                if (completely_empty) {
                        LOG("Removing completely empty row %d\n", rows);
                        if (rows < (workspace->rows - 1))
                                move_rows_from(conn, workspace, rows+1);
                        else {
                                for (int cols = 0; cols < workspace->cols; cols++)
                                        free_container(conn, workspace, cols, rows);
                        }
                        shrink_table_rows(workspace);

                        if (workspace->current_row >= workspace->rows)
                                workspace->current_row = workspace->rows - 1;
                } else rows++;
        }

        /* Boundary checking for current_col and current_row */
        if (current_col >= c_ws->cols)
                current_col = c_ws->cols-1;

        if (current_row >= c_ws->rows)
                current_row = c_ws->rows-1;

        if (CUR_CELL->currently_focused != NULL)
                set_focus(conn, CUR_CELL->currently_focused, true);
}

/*
 * Fixes col/rowspan (makes sure there are no overlapping windows, obeys borders).
 *
 */
void fix_colrowspan(xcb_connection_t *conn, Workspace *workspace) {
        LOG("Fixing col/rowspan\n");

        FOR_TABLE(workspace) {
                Container *con = workspace->table[cols][rows];
                if (con->colspan > 1) {
                        LOG("gots one with colspan %d (at %d c, %d r)\n", con->colspan, cols, rows);
                        while (con->colspan > 1 &&
                               (!cell_exists(cols + (con->colspan-1), rows) ||
                                workspace->table[cols + (con->colspan - 1)][rows]->currently_focused != NULL))
                                con->colspan--;
                        LOG("fixed it to %d\n", con->colspan);
                }
                if (con->rowspan > 1) {
                        LOG("gots one with rowspan %d (at %d c, %d r)\n", con->rowspan, cols, rows);
                        while (con->rowspan > 1 &&
                               (!cell_exists(cols, rows + (con->rowspan - 1)) ||
                                workspace->table[cols][rows + (con->rowspan - 1)]->currently_focused != NULL))
                                con->rowspan--;
                        LOG("fixed it to %d\n", con->rowspan);
                }
        }
}
