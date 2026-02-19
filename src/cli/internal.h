/**
 * CLI module internal header.
 *
 * Subcommand entry points for quadrature-cli.
 * Each returns a process exit code: 0 success, 1 error, 130 cancelled.
 */

#ifndef QUADRATURE_CLI_INTERNAL_H
#define QUADRATURE_CLI_INTERNAL_H

int cli_indexer(int argc, char** argv);
int cli_setup_rt(int argc, char** argv);

#endif // QUADRATURE_CLI_INTERNAL_H
