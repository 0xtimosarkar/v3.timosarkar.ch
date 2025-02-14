#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
  PRE    = 1 << 0,
  CODE   = 1 << 1,
  EM     = 1 << 2,
  STRONG = 1 << 3,
  STRIKE = 1 << 4,
  H1     = 1 << 5,
  H2     = 1 << 6,
  H3     = 1 << 7,
  LIST   = 1 << 8,
  QUOTE  = 1 << 9,
};

static void panic(const char *msg) {
  fprintf(stderr, "error: %s\n", msg);
  exit(1);
}

static char* copy_until(char *dst, char *src, char *chars) {
  while (*src && !strchr(chars, *src)) {
    if (*src == '\\') { *dst++ = *src++; }
    *dst++ = *src++;
  }
  *dst = '\0';
  return src;
}

static bool consume(char **p, char *expect) {
  char *q = *p;
  while (*expect) {
    if (*q++ != *expect++) { return false; }
  }
  *p = q;
  return true;
}

static void write_fp(FILE *out, FILE *in) {
  int chr;
  while ((chr = fgetc(in)) != EOF) { fputc(chr, out); }
}

static void write_b64_fp(FILE *out, FILE *in) {
  static char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  int n;
  do {
    unsigned char b[3] = {0};
    n = fread(b, 1, 3, in);
    if (n == 0) { break; }
    unsigned x = (b[0] << 16) | (b[1] << 8) | b[2];
    fputc(        t[(x >> 18) & 0x3f],       out);
    fputc(        t[(x >> 12) & 0x3f],       out);
    fputc(n > 1 ? t[(x >>  6) & 0x3f] : '=', out);
    fputc(n > 2 ? t[(x >>  0) & 0x3f] : '=', out);
  } while (n == 3);
}

static int write_text(FILE *fp, char *text, int flags);

static int write_embedded(FILE *fp, char **p, int flags) {
  char name[512];
  *p = copy_until(name, *p, "\n :]*");
  FILE *in = fopen(name, "rb");
  if (in) {
    if (strstr(name, ".png") || strstr(name, ".jpg") || strstr(name, ".gif")) {
      fprintf(fp, "<img src=\"data:image;base64, ");
      write_b64_fp(fp, in);
      fprintf(fp, "\"/>");
    } else {
      write_fp(fp, in);
    }
    fclose(in);
    return flags;
  }
  fputc('@', fp);
  return write_text(fp, name, flags);
}

static int write_link(FILE *fp, char **p, int flags) {
  char text[512], url[512];
  *p = copy_until(text, *p, "]");
  if (consume(p, "](")) {
    *p = copy_until(url, *p, ")");
    consume(p, ")");
    fprintf(fp, "<a href=\""); write_text(fp, url, PRE); fprintf(fp, "\">");
    write_text(fp, text, flags);
    fprintf(fp, "</a>");
    return flags;
  }
  fputc('[', fp);
  return write_text(fp, text, flags);
}

static int edge(FILE *fp, int flags, int f, char *tag) {
  if (flags & f) {
    fprintf(fp, "</%s>", tag);
    return flags & ~f;
  }
  fprintf(fp, "<%s>", tag);
  return flags | f;
}

static int write_text(FILE *fp, char *text, int flags) {
  for (char *p = text;; p++) {
top:
    if (~flags & PRE) {
      if (consume(&p,    "`")) { flags = edge(fp, flags, CODE,     "code"); goto top; }
      if (~flags & CODE) {
        if (consume(&p, "~~")) { flags = edge(fp, flags, STRIKE, "strike"); goto top; }
        if (consume(&p,  "*")) { flags = edge(fp, flags, EM,         "em"); goto top; }
        if (consume(&p,  "_")) { flags = edge(fp, flags, STRONG, "strong"); goto top; }
        if (consume(&p,  "@")) { flags = write_embedded(fp, &p, flags);     goto top; }
        if (consume(&p,  "[")) { flags = write_link(fp, &p, flags);         goto top; }
      }
    }

    if (*p == '\\') { p++; }
    switch (*p) {
      case '\0': return flags;
      case '<' : fprintf(fp, "&lt;");   break;
      case '>' : fprintf(fp, "&gt;");   break;
      case '&' : fprintf(fp, "&amp;");  break;
      case '"' : fprintf(fp, "&quot;"); break;
      case '\'': fprintf(fp, "&apos;"); break;
      default  : fputc(*p, fp);         break;
    }
  }
}

static int process_line(FILE *fp, char *line, int flags) {
  /* code block */
  if (consume(&line, "```")) { return edge(fp, flags, PRE, "pre"); }
  if (flags & PRE) { return write_text(fp, line, flags); }

  /* skip whitespace */
  while (isspace(*line)) { line++; }

  /* quote */
  if (consume(&line, ">")) {
    if (~flags & QUOTE) { flags = edge(fp, flags, QUOTE, "blockquote"); }
    while (isspace(*line)) { line++; }
  } else if (flags & QUOTE && !*line) {
    flags = edge(fp, flags, QUOTE, "blockquote");
  }

  /* list */
  if (consume(&line, "* ")) {
    if (~flags & LIST) { flags = edge(fp, flags, LIST, "ul"); }
    fprintf(fp, "<li>");
  } else if (flags & LIST && !*line) {
    flags = edge(fp, flags, LIST, "ul");
  }

  /* new paragraph */
  if (!*line) { fprintf(fp, "<p>"); }

  /* header */
  if (consume(&line,   "# ")) { flags = edge(fp, flags, H1, "h1"); }
  if (consume(&line,  "## ")) { flags = edge(fp, flags, H2, "h2"); }
  if (consume(&line, "### ")) { flags = edge(fp, flags, H3, "h3"); }

  /* write text */
  flags = write_text(fp, line, flags);

  /* finish header */
  if (flags & H1) { flags = edge(fp, flags, H1, "h1"); }
  if (flags & H2) { flags = edge(fp, flags, H2, "h2"); }
  if (flags & H3) { flags = edge(fp, flags, H3, "h3"); }

  return flags;
}

static void process_file(const char *filename) {
    FILE *in = fopen(filename, "rb");
    if (!in) {
        fprintf(stderr, "Failed to open input file: %s\n", filename);
        return;
    }

    // Replace stdout with file output for each HTML file
    char output_filename[1024];
    snprintf(output_filename, sizeof(output_filename), "%s.html", filename);
    FILE *out = fopen(output_filename, "wb");
    if (!out) {
        fprintf(stderr, "Failed to open output file: %s\n", output_filename);
        fclose(in);
        return;
    }

    fprintf(out, "<html><head><meta charset=\"utf-8\"><style>");
    FILE *css = fopen("custom.css", "rb"); // assuming custom.css is the name of your CSS file
    if (css) {
        write_fp(out, css);
        fclose(css);
    } else {
        fprintf(out,
                "body{margin:60 auto;max-width:750px;line-height:1.6;"
                "font-family:Open Sans,Arial;color:#444;padding:0 10px;}"
                "h1,h2,h3{line-height:1.2;padding-top: 14px;}");
    }
    fprintf(out, "</style></head><body>");

    // Custom header template
    fprintf(out, "<header><p>Custom Header</p></header>");

    char line[4096];
    int flags = 0;
    while (fgets(line, sizeof(line), in)) {
        flags = process_line(out, line, flags);
    }

    // Custom footer template
    fprintf(out, "<footer>custom footer injected from odie automatically</footer></body></html>\n");

    // Close files
    fclose(in);
    fclose(out);
}

static void process_directory(const char *dir_name) {
    DIR *dir;
    struct dirent *entry;

    dir = opendir(dir_name);
    if (!dir) {
        fprintf(stderr, "Cannot open directory: %s\n", dir_name);
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", dir_name, entry->d_name);
        struct stat statbuf;
        stat(path, &statbuf);

        if (S_ISDIR(statbuf.st_mode)) {
            if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
                // Recursive call for subdirectories
                process_directory(path);
            }
        } else {
            if (strstr(entry->d_name, ".md")) {
                process_file(path);
            }
        }
    }
    closedir(dir);
}

int main(int argc, char **argv) {
    process_directory(".");
    FILE *fp = fopen("index.html", "w");

    if (fp == NULL) {
        printf("Error opening file!\n");
        return 1;
    }

    fprintf(fp, "<!-- odie index page - autogenerated -->\n");
    fprintf(fp, "<link rel=icon href=data:>\n");
    fprintf(fp, "<meta name=viewport content=width=1%%>\n");
    fprintf(fp, "<pre style=font:unset>\n");
    fprintf(fp, "Hi, I'm <a href=a>Name</a>! I like <a href=r>changeme</a>, changeme, changeme,\n");
    fprintf(fp, "changeme, changeme, <a href=s>changeme</a>, and changeme\n");
    fprintf(fp, "\n");
    fprintf(fp, "Please sign my <a href=g>Guest Book</a>\n");
    fprintf(fp, "\n");
    fprintf(fp, "site@ts.cli.rs\n");
    fprintf(fp, "\n");
    fprintf(fp, "CV\n");
    fprintf(fp, "\n");
    fprintf(fp, "Jobtitle - Companyname, 'Year-\n");
    fprintf(fp, "Jobtitle - Companyname, 'Year-Year\n");
    fprintf(fp, "Jobtitle - Companyname, 'Year-Year\n");
    fprintf(fp, "Jobtitle - Companyname, 'Year-Year\n");
    fprintf(fp, "\n");
    fprintf(fp, "Blog\n");
    fprintf(fp, "\n");

    // Open current directory
    DIR *dir;
    struct dirent *ent;
    if ((dir = opendir(".")) != NULL) {
        // Iterate over directory entries
        while ((ent = readdir(dir)) != NULL) {
            // Check if file ends with ".md"
            if (strlen(ent->d_name) > 3 && strcmp(ent->d_name + strlen(ent->d_name) - 3, ".md") == 0) {
                // Generate HTML anchor tag for Markdown file with ".html" extension
                char *html_filename = malloc(strlen(ent->d_name) + 2); // Adding 2 for ".html" and null terminator
                if (html_filename != NULL) {
                    strcpy(html_filename, ent->d_name);
                    strcpy(html_filename + strlen(html_filename) - 2, "html");
                    fprintf(fp, "<a href=\"%s\">%s</a>\n", html_filename, ent->d_name);
                    free(html_filename);
                } else {
                    fprintf(stderr, "Memory allocation error!\n");
                    return 1;
                }
            }
        }
        closedir(dir);
    } else {
        // Could not open directory
        perror("");
        return 1;
    }
    fprintf(fp, "</pre>\n");
    fclose(fp);
  
    return 0;
}
