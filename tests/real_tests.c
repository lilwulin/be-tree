#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <valgrind/callgrind.h>
#include <gsl/gsl_histogram.h>
#include <gsl/gsl_sort.h>
#include <gsl/gsl_statistics.h>

#include "betree.h"
#include "debug.h"
#include "hashmap.h"
#include "utils.h"

#define MAX_EXPRS 5000
#define MAX_EVENTS 1000
#define DEFAULT_SEARCH_COUNT 10

struct events {
    size_t count;
    char** events;
};

extern bool MATCH_NODE_DEBUG;

void add_event(char* event, struct events* events)
{
    if(events->count == 0) {
        events->events = calloc(1, sizeof(*events->events));
        if(events == NULL) {
            fprintf(stderr, "%s calloc failed", __func__);
            abort();
        }
    }
    else {
        char** new_events
            = realloc(events->events, sizeof(*new_events) * ((events->count) + 1));
        if(new_events == NULL) {
            fprintf(stderr, "%s realloc failed", __func__);
            abort();
        }
        events->events = new_events;
    }
    events->events[events->count] = strdup(event);
    events->count++;
}

char* strip_chars(const char* string, const char* chars)
{
    char* new_string = malloc(strlen(string) + 1);
    int counter = 0;

    for(; *string; string++) {
        if(!strchr(chars, *string)) {
            new_string[counter] = *string;
            ++counter;
        }
    }
    new_string[counter] = 0;
    return new_string;
}

int event_parse(const char* text, struct event** event);

size_t read_betree_events(struct config* config, struct events* events)
{
    FILE* f = fopen("betree_events", "r");
    size_t count = 0;

    char line[22000]; // Arbitrary from what I've seen
    while(fgets(line, sizeof(line), f)) {
        if(MAX_EVENTS != 0 && events->count == MAX_EVENTS) {
            break;
        }

        struct event* event;
        if(event_parse(line, &event) != 0) {
            fprintf(stderr, "Can't parse event %zu: %s", events->count + 1, line);
            abort();
        }

        fill_event(config, event);
        if(!validate_event(config, event)) {
            abort();
        }
        add_event(line, events);
        count++;
    }
    fclose(f);
    return count;
}

size_t read_betree_exprs(struct betree* tree)
{
    FILE* f = fopen("betree_exprs", "r");

    //char* lines[MAX_EXPRS];
    char line[10000]; // Arbitrary from what I've seen
    size_t count = 0;
    while(fgets(line, sizeof(line), f)) {
        if(!betree_insert(tree, count, line)) {
            printf("Can't insert expr: %s\n", line);
            abort();
        }
        count++;
        if(MAX_EXPRS != 0 && count == MAX_EXPRS) {
            break;
        }
    }

    /*
    while(fgets(line, sizeof(line), f)) {
        lines[count] = strdup(line);
        count++;
        if(MAX_EXPRS != 0 && count == MAX_EXPRS) {
            break;
        }
    }

    betree_insert_all(tree, count, (const char**)lines);
    */

    fclose(f);
    return count;
}

void read_betree_defs(struct betree* tree)
{
    FILE* f = fopen("betree_defs", "r");

    char line[LINE_MAX];
    while(fgets(line, sizeof(line), f)) {
        betree_add_domain(tree, line);
    }

    fclose(f);
}

int compare_int( const void* a, const void* b )
{
    if( *(int*)a == *(int*)b ) return 0;
    return *(int*)a < *(int*)b ? -1 : 1;
}

int main(int argc, char** argv)
{
    size_t search_count = DEFAULT_SEARCH_COUNT;
    if(argc > 1) {
        search_count = atoi(argv[1]);
    }
    if(access("betree_defs", F_OK) == -1 || access("betree_events", F_OK) == -1
        || access("betree_exprs", F_OK) == -1) {
        fprintf(stderr, "Missing files, skipping the tests");
        return 0;
    }
    struct timespec start, insert_done, gen_event_done, search_done;

    // Init
    struct betree* tree = betree_make();
    read_betree_defs(tree);

    clock_gettime(CLOCK_MONOTONIC_RAW, &start);

    // Insert
    size_t expr_count = read_betree_exprs(tree);

    clock_gettime(CLOCK_MONOTONIC_RAW, &insert_done);
    uint64_t insert_us = (insert_done.tv_sec - start.tv_sec) * 1000000
        + (insert_done.tv_nsec - start.tv_nsec) / 1000;
    printf("    Insert took %" PRIu64 "\n", insert_us);

    struct events events = { .count = 0, .events = NULL };
    size_t event_count = read_betree_events(tree->config, &events);

    uint64_t evaluated_sum = 0;
    uint64_t matched_sum = 0;
    uint64_t memoized_sum = 0;
    uint64_t shorted_sum = 0;

    const size_t search_us_count = search_count * events.count;
    double search_us_data[search_us_count];

    /*MATCH_NODE_DEBUG = true;*/

    CALLGRIND_START_INSTRUMENTATION;

    size_t search_us_i = 0;
    
    for(size_t j = 0; j < search_count; j++) {
        for(size_t i = 0; i < events.count; i++) {
            clock_gettime(CLOCK_MONOTONIC_RAW, &gen_event_done);

            char* event = events.events[i];
            struct report* report = make_report();
            betree_search(tree, event, report);

            clock_gettime(CLOCK_MONOTONIC_RAW, &search_done);

            uint64_t search_us = (search_done.tv_sec - gen_event_done.tv_sec) * 1000000
                + (search_done.tv_nsec - gen_event_done.tv_nsec) / 1000;

            search_us_data[search_us_i] = (double)search_us;

            evaluated_sum += report->evaluated;
            matched_sum += report->matched;
            memoized_sum += report->memoized;
            shorted_sum += report->shorted;
            free_report(report);
            search_us_i++;
        }
        printf("Finished run %zu/%zu\n", j, search_count);
    }

    CALLGRIND_STOP_INSTRUMENTATION;
    CALLGRIND_DUMP_STATS;

    double evaluated_average = (double)evaluated_sum / (double)MAX_EVENTS;
    double matched_average = (double)matched_sum / (double)MAX_EVENTS;
    double memoized_average = (double)memoized_sum / (double)MAX_EVENTS;
    double shorted_average = (double)shorted_sum / (double)MAX_EVENTS;
    printf("%zu searches, %zu expressions, %zu events, %zu preds. Evaluated %.2f, matched %.2f, memoized %.2f, shorted %.2f\n",
        search_us_count,
        expr_count,
        event_count,
        tree->config->pred_map->pred_count,
        evaluated_average / search_count,
        matched_average / search_count,
        memoized_average / search_count,
        shorted_average / search_count);

    double search_us_min = gsl_stats_min(search_us_data, 1, search_us_count);
    double search_us_max = gsl_stats_max(search_us_data, 1, search_us_count);
    double search_us_mean = gsl_stats_mean(search_us_data, 1, search_us_count);
    gsl_sort(search_us_data, 1, search_us_count);
    double search_us_90 = gsl_stats_quantile_from_sorted_data(search_us_data, 1, search_us_count, 0.90);
    double search_us_95 = gsl_stats_quantile_from_sorted_data(search_us_data, 1, search_us_count, 0.95);
    double search_us_99 = gsl_stats_quantile_from_sorted_data(search_us_data, 1, search_us_count, 0.99);

    printf("Min: %.1f, Mean: %.1f, Max: %.1f, 90: %.1f, 95: %.1f, 99: %.1f\n", search_us_min, search_us_mean, search_us_max, search_us_90, search_us_95, search_us_99);

    printf("| %lu | %.1f | %.1f | %.1f | %.1f | %.1f | %.1f | |\n", insert_us, search_us_min, search_us_mean, search_us_max, search_us_90, search_us_95, search_us_99);

    // DEBUG
    write_dot_file(tree->config, tree->cnode);
    // DEBUG
    
    free(events.events);
    betree_free(tree);
    return 0;
}

