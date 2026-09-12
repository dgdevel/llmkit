#ifndef HTMLMD_H
#define HTMLMD_H

#include <stddef.h>

/*
 * htmlmd -- dependency-free HTML-to-markdown conversion with a simplified
 * readability pass, used by the `llmkit mcp online_fetch` built-in tool.
 *
 * Pipeline:
 *   1. parse the document into a minimal DOM (forgiving: implicit closes
 *      for <li>/<p>/<tr>/<td>, quoted attribute values, entity decoding)
 *   2. readability: drop boilerplate subtrees (script/style/nav/header/
 *      footer/aside/forms/...) and prefer <article>, then <main>, then
 *      <body> as the content root
 *   3. render the remaining tree as markdown: headings, paragraphs,
 *      nested lists, links, images, fenced code blocks, blockquotes,
 *      horizontal rules and simple tables
 */

/* Convert an HTML document to a markdown document. Returns a malloc'd
 * NUL-terminated string ("" for empty input). The page <title>, when
 * present, is prepended as a level-1 heading. Never returns NULL. */
char *htmlmd_convert(const char *html);

/* Match result: text content of one element (tags stripped, whitespace
 * collapsed, entities decoded) plus the value of one requested attribute
 * (NULL if the element lacks it or none was requested). */
typedef struct {
    char *text;
    char *attr;
} htmlmd_match;

/* Find every element whose class attribute contains `class_name` (as a
 * whitespace-separated token) and return their text/attribute pairs in
 * document order. want_attr may be NULL. Returns a malloc'd array of
 * *out_count entries (NULL when none found). */
htmlmd_match *htmlmd_find_by_class(const char *html, const char *class_name, const char *want_attr,
                                   int *out_count);

void htmlmd_matches_free(htmlmd_match *matches, int count);

#endif /* HTMLMD_H */
