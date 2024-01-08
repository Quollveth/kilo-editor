#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <stdarg.h>
#include <fcntl.h>

/*** defines ***/

#define QUILLO_VERSION "1"
#define CTRL_KEY(k) ((k) & 0x1f)
#define QUILLO_TAB_STOP 8
#define QUILLO_MESSAGE_DURATION 5 //in seconds
#define QUILLO_QUIT_TIMES 2 //how many times ctrl q has to be pressed before quitting without saving

enum editorKeys {
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    PAGE_UP,
    PAGE_DOWN,
    HOME_KEY,
    END_KEY,
    DELETE_KEY
};

enum editorHighlight {
    HL_NORMAL = 0,
    HL_MATCH,
    //syntax
    HL_NUMBER,
    HL_STRING,
    HL_COMMENT,
    HL_MLCOMMENT,
    HL_KEYWORD1,
    HL_KEYWORD2
};

#define HL_HIGHLIGHT_NUMBERS (1<<0)
#define HL_HIGHLIGHT_STRINGS (1<<1)

/*** data ***/

typedef struct erow{
    int idx;
    int size;
    int rsize;
    char *chars;
    char *render;
    unsigned char *hl;
    int hlOpenComment;
} erow;

struct editorSyntax{
    char *filetype;
    char **filematch;
    char **keywords;
    char *singleLineCommentStart;
    char *mlCommentStart;
    char *mlCommentEnd;
    int flags;
};

struct editorConfig {
    int cx, cy; //cursor position
    int screenrows, screencols; //screen size
    int rowoffset, coloffset; //scrolling
    int numrows;
    erow *row;    
    int rx; //index into render, for tabs
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
    int dirty; //has the file been modified
    struct termios org_termios;
    struct editorSyntax *syntax;
};

struct editorConfig E;


/*** filetypes ***/

/* 
each language needs to have a editorSyntax struct inside the HLDB array defining it's syntax for highlightinh
filetype is the name that will show in the editor with the currently selected language
filematch is a string array containing all the file extensions it uses
keywords is a string array with all the keywords, keywords ending with | receive a different color
singleLineCommentStart is a string defining the sequence that being a single line comment
mlCommentStart and mlCommentEnd are strings defining the sequences that begin and end multi line comments
*/

char *C_HL_extensions[] = { ".c", ".h", ".cpp", NULL };
char *C_HL_keywords[] = { 
    "switch", "if", "while", "for", "break", "continue", "return", "else", "struct", "union", "typedef", "static", "enum", "class", "case",
    "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|", "void|", NULL
};

struct editorSyntax HLDB[] = {
    {
        "c",
        C_HL_extensions,
        C_HL_keywords,
        "//", "/*", "*/",
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
    },
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/*** prototypes ***/

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback)(char *, int));

/*** terminal ***/

void die(const char *s){
    //clear the screen
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    //send error
    perror(s);
    exit(1);
}

void disableRawMode(){
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.org_termios) == -1) die("tcsetattr");
}

void enableRawMode(){
    if(tcgetattr(STDIN_FILENO, &E.org_termios) == -1) die("tcgetattr");

    atexit(disableRawMode);

    struct termios raw = E.org_termios;
    //bit masks, BRKINT, INPCK, ISTRIP and CS8 are probably not needed for modern terminals
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);

    raw.c_cc[VMIN] = 0; //minimum number of bytes before return
    raw.c_cc[VTIME] = 1; //maximum time before return, 100ms

    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

int editorReadKey(){
    int nread;
    char c;
    while((nread = read(STDIN_FILENO, &c, 1)) != 1){
        if(
            nread == -1 //read failure
            && errno != EAGAIN //not a timeout (Cygwin)
        ) die("read");
    }
    if(c != '\x1b'){
        return c;
    }

    //handle escape sequences
    char seq[3];
    
    if(read(STDIN_FILENO, &seq[0],1) != 1) return '\x1b';
    if(read(STDIN_FILENO, &seq[1],1) != 1) return '\x1b';
    
    if(seq[0] != '[' && seq[0] != 'O')return '\x1b';

    if(seq[0]=='O'){
        switch (seq[1]){
        case 'H': return HOME_KEY;
        case 'F': return END_KEY;
        }
    }

    if(isdigit(seq[1])){
        if(read(STDIN_FILENO, &seq[2],1) != 1) return '\x1b';

        if(seq[2]=='~'){
            switch (seq[1]) {
                case '1': return HOME_KEY;
                case '4': return END_KEY;
                case '3': return DELETE_KEY;
                case '5': return PAGE_UP;
                case '6': return PAGE_DOWN;
                case '7': return HOME_KEY;
                case '8': return END_KEY;
            }
        }
    }

    switch (seq[1]) {
        case 'A': return ARROW_UP;
        case 'B': return ARROW_DOWN;
        case 'C': return ARROW_RIGHT;
        case 'D': return ARROW_LEFT;
        case 'H': return HOME_KEY;
        case 'F': return END_KEY;
    }

    return '\x1b';
}

int getCursorPosition(int *rows, int *cols){
    char buf[32];
    unsigned int i = 0;

    if(write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while(i < sizeof(buf)-1){
        if(read(STDIN_FILENO, &buf[i],1) != 1) break;
        if(buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

    if(buf[0] != '\x1b' || buf[1] != '[') return -1;
    if(sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

int getWindowSize(int *rows, int *cols){
    struct winsize ws;

    if(ioctl(STDOUT_FILENO,TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0){
        //ioctl failed, fallback method
        if(write(STDOUT_FILENO, "x1b[999C\x1b[999B",12) != 12) return -1;
        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** syntax highlighting ***/

int editorSyntaxToColor(int hl){
    switch (hl){
        case HL_NUMBER: return 31;
        case HL_MATCH: return 34;
        case HL_STRING: return 35;
        case HL_MLCOMMENT:
        case HL_COMMENT: return 36;
        case HL_KEYWORD1: return 31;
        case HL_KEYWORD2: return 32;
        default: return 37;
    }
}

int isSeparator(int c){
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];",c) != NULL;
}

void editorUpdateSyntax(erow *row){
    row->hl = realloc(row->hl, row->rsize);
    memset(row->hl, HL_NORMAL, row->rsize);

    if(E.syntax == NULL) return;

    int prevSep = 1;
    int inString = 0;
    int inComment = (row->idx > 0 && E.row[row->idx-1].hlOpenComment);

    char **keywords = E.syntax->keywords;

    char *scs = E.syntax->singleLineCommentStart;
    char *mcs = E.syntax->mlCommentStart;
    char *mce = E.syntax->mlCommentEnd;

    int scsLen = scs?strlen(scs):0;
    int mcsLen = mcs?strlen(mcs):0;    
    int mceLen = mce?strlen(mce):0;

    int i=0;
    while(i < row->rsize){
        char c = row->render[i];
        unsigned char prevHl = (i>0)?row->hl[i-1] : HL_NORMAL;

    //single line comments
        if(scsLen && !inString && !inComment){
            if(!strncmp(&row->render[i],scs,scsLen)){
                memset(&row->hl[i], HL_COMMENT, row->rsize-i);
                break;
            }
        }
    //multi line comments
    if(mcsLen && mceLen && !inString){
        if(inComment){
            row->hl[i] = HL_MLCOMMENT;
            if(!strncmp(&row->render[i],mce,mceLen)){
                memset(&row->hl[i],HL_MLCOMMENT,mceLen);
                i += mceLen;
                inComment = 0;
                prevSep = 1;
                continue;
            }
            i++;
            continue;
        }
        else if(!strncmp(&row->render[i],mcs,mcsLen)){
            memset(&row->hl[i],HL_MLCOMMENT,mcsLen);
            i += mcsLen;
            inComment = 1;
            continue;
        }
    }

    //strings
        if(E.syntax->flags & HL_HIGHLIGHT_STRINGS){
            if(inString){
                row->hl[i] = HL_STRING;
                if(c == '\\' && i+1 < row->rsize){ //escaped characters inside string
                    row->hl[i+1] = HL_STRING;
                    i += 2;
                    continue;
                }
                if(c == inString) inString = 0;
                i++;
                prevSep = 1;
                continue;
            }
            else {
                if(c == '"' || c == '\''){
                    inString = c;
                    row->hl[i] = HL_STRING;
                    i++;
                    continue;
                }
            }
        }
    //numbers
        if(E.syntax->flags & HL_HIGHLIGHT_NUMBERS){
            if((isdigit(c) && (prevSep || prevHl == HL_NUMBER)) || (c == '.' && prevHl == HL_NUMBER)) {
            row->hl[i] = HL_NUMBER;
            i++;
            prevSep = 0;
            continue;
            }
        }
    //keywords
        if(prevSep){
            for(int j=0;keywords[j];j++){
                int klen = strlen(keywords[j]);
                int kw2 = keywords[j][klen-1] == '|';
                if(kw2) klen--;

                if(!strncmp(&row->render[i],keywords[j],klen) && isSeparator(row->render[i+klen])){
                    memset(&row->hl[i],kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
                    i += klen;
                    break;
                }
                if(keywords[j] != NULL){
                    prevSep = 0;
                    continue;
                }
            }            
        }
    //end
        prevSep = isSeparator(c);
        i++;
    }

    int changed = (row->hlOpenComment != inComment);
    row->hlOpenComment = inComment;
    if(changed && row->idx + 1 < E.numrows){
        editorUpdateSyntax(&E.row[row->idx+1]);
    }
}

void editorSelectSyntaxHL(){
    E.syntax = NULL;
    if(E.filename == NULL) return;

    char *ext = strchr(E.filename, '.');

    for(unsigned int j = 0; j < HLDB_ENTRIES; j++){
        struct editorSyntax *s = &HLDB[j];
        unsigned int i=0;
        while(s->filematch[i]){
            int is_ext = (s->filematch[i][0]=='.');
            if((is_ext && ext && !strcmp(ext,s->filematch[i])) || (!is_ext && strstr(E.filename, s->filematch[i]))){
                E.syntax = s;

                for(int filerow = 0;filerow < E.numrows; filerow++){
                    editorUpdateSyntax(&E.row[filerow]);
                }

                return;
            }
            i++;
        }
    }
}

/*** row operations ***/

int editorRowCxToRx(erow *row, int cx){
    int rx = 0;
    int j;
    for(j=0; j<cx; j++){
        if(row->chars[j] == '\t')
            rx += (QUILLO_TAB_STOP -1) - (rx % QUILLO_TAB_STOP);
        rx++;
    }
    return rx;
}

int editorRowRxToCx(erow *row, int rx){
    int cur_rx = 0;
    int cx;
    for(cx = 0; cx < row->size; cx++){
        if(row->chars[cx] == '\t'){
            cur_rx += (QUILLO_TAB_STOP - 1) - (cur_rx % QUILLO_TAB_STOP);
        }
        cur_rx++;
        if(cur_rx > rx) return cx;
    }
    return cx;
}

void editorUpdateRow(erow *row){
    int tabs = 0;
    int j;
    for(j=0; j<row->size; j++){
        if(row->chars[j] == '\t') tabs++;
    }

    free(row->render);
    row->render = malloc(row->size + tabs* (QUILLO_TAB_STOP-1) +1);

    int idx = 0;
    for(j=0; j<row->size; j++){
        if(row->chars[j]!='\t'){
            row->render[idx++] = row->chars[j];
            continue;
        }
        row->render[idx++] = ' ';
        while(idx % QUILLO_TAB_STOP != 0) row->render[idx++] = ' ';
    }
    row->render[idx] = '\0';
    row->rsize = idx;

    editorUpdateSyntax(row);
}

void editorInsertRow(int at,char *s, size_t len){
    if(at < 0 || at > E.numrows) return;
    
    E.row = realloc(E.row, sizeof(erow) * (E.numrows+1));
    memmove(&E.row[at+1], &E.row[at], sizeof(erow)*(E.numrows - at));
    for(int j=at+1; j<=E.numrows;j++) E.row[j].idx++;

    E.row[at].idx = at;
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars,s,len);
    E.row[at].chars[len] = '\0';
    E.row[at].render = NULL;
    E.row[at].rsize = 0;
    E.row[at].hl = NULL;
    E.row[at].hlOpenComment = 0;

    editorUpdateRow(&E.row[at]);
    E.numrows++;
    E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c){
    if(at < 0 || at > row->size) at = row->size;
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at+1], &row->chars[at],row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);
    E.dirty++;
}

void editorInsertNewLine(){
    if(E.cx == 0){
        editorInsertRow(E.cy, "", 0);
        E.cy++;
        E.cx = 0;
        return;
    }
    erow *row = &E.row[E.cy];
    editorInsertRow(E.cy+1, &row->chars[E.cx], row->size - E.cx);
    row = &E.row[E.cy];
    row->size = E.cx;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.cy++;
    E.cx = 0;
}

void editorRowDelChar(erow *row, int at){
    if(at < 0 || at >= row->size) return;
    memmove(&row->chars[at], &row->chars[at +1],row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

void editorFreeRow(erow *row){
    free(row->chars);
    free(row->render);
    free(row->hl);
}

void editorDelRow(int at){
    if(at < 0 || at >= E.numrows) return;
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at],&E.row[at +1],E.numrows-at);
    for(int j=at; j<E.numrows-1;j++) E.row[j].idx--;
    E.numrows--;
    E.dirty++;
}

void editorRowAppendString(erow *row, char *s, int len){
    row->chars = realloc(row->chars,row->size + len +1);
    memcpy(&row->chars[row->size],s,len);
    row->size += len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;    
}

/*** editor operations ***/

void editorInsertChar(int c){
    if(E.cy == E.numrows){
        editorInsertRow(E.numrows,"",0);
    }
    editorRowInsertChar(&E.row[E.cy],E.cx,c);
    E.cx++;
}

void editorDelChar(){
    if(E.cy == E.numrows) return;
    if(E.cx == 0 && E.cy == 0) return;

    erow *row = &E.row[E.cy];

    if(E.cx > 0){
        editorRowDelChar(row, E.cx-1);
        E.cx--;
        return;
    }
    E.cx = E.row[E.cy-1].size;
    editorRowAppendString(&E.row[E.cy-1],row->chars,row->size);
    editorDelRow(E.cy);
    E.cy--;
}

/*** file i/o ***/

void editorOpen(char *filename){
    FILE *fp = fopen(filename, "r");
    if(!fp) die("fopen");

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;

    while((linelen = getline(&line, &linecap, fp)) != -1){
        while(linelen > 0 && (line[linelen-1] == '\n' || line[linelen-1] == '\r')) linelen--;
        editorInsertRow(E.numrows,line,linelen);
    }    

    free(E.filename);
    E.filename = strdup(filename);
    editorSelectSyntaxHL();

    free(line);
    fclose(fp);

    E.dirty = 0;
}

char *editorRowsToString(int *buflen){
    int totlen = 0;
    int j;
    for(j=0; j<E.numrows; j++){
        totlen += E.row[j].size +1;
    }
    *buflen = totlen;

    char *buf = malloc(totlen);
    char *p = buf;
    for(j=0; j<E.numrows; j++){
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }

    return buf;
}

void editorSave(){
    if(E.filename == NULL){
        E.filename = editorPrompt("Save as: %s", NULL);
    }

    if(E.filename == NULL){
        editorSetStatusMessage("Save aborted by user");
        return;
    }

    int len;
    char *buf = editorRowsToString(&len);

    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if(fd == -1) goto error;

    if(ftruncate(fd, len) == -1) goto error;

    if(write(fd, buf, len) != len) goto error;

    close(fd);
    free(buf);
    E.dirty = 0;
    editorSelectSyntaxHL();
    editorSetStatusMessage("%d bytes written to disk",len);
    return;

    error:
    close(fd);
    free(buf);
    editorSetStatusMessage("I/O Error while saving: %s",strerror(errno));
}

/*** search ***/

void editorFindCallback(char *query, int key){
    static int lastMatch = -1;
    static int direction = 1;
    static int savedHlLine;
    static char *savedHl = NULL;

    if(savedHl){
        memcpy(E.row[savedHlLine].hl,savedHl,E.row[savedHlLine].rsize);
        free(savedHl);
        savedHl = NULL;
    }

    switch(key){
        case '\r':
        case '\x1b':
            lastMatch = -1;
            direction = 1;
            return;
        case ARROW_DOWN:
        case ARROW_RIGHT:
            direction = 1;
            break;
        case ARROW_UP:
        case ARROW_LEFT:
            direction = -1;
            break;
        default:
            lastMatch = -1;
            direction = 1;
    }
    
    if(lastMatch == -1) direction = 1;
    int current = lastMatch;
    int i;
    for(i=0;i<E.numrows;i++){
        current += direction;
        if(current == -1) current = E.numrows-1;
        else if(current == E.numrows) current = 0;

        erow *row = &E.row[current];
        char *match = strstr(row->render, query);
        if(match){
            lastMatch = current;
            E.cy = current;
            E.cx = editorRowRxToCx(row, match - row->render);
            E.rowoffset = E.numrows;

            savedHlLine = current;
            savedHl = malloc(row->rsize);
            memcpy(savedHl, row->hl, row->rsize);
            memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
            break;
        }
    }
}

void editorFind(){
    int savedcx = E.cx;
    int savedcy = E.cy;
    int savedcoloff = E.coloffset;
    int savedrowoff = E.rowoffset;

    char *query = editorPrompt("Search: %s", editorFindCallback);

    if(query){
        free(query);
        return;
    }
    E.cx = savedcx;
    E.cy = savedcy;
    E.coloffset = savedcoloff;
    E.rowoffset = savedrowoff;
}

/*** append buffer ***/

struct abuf{
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len){
    char *new = realloc(ab->b, ab->len + len);

    if(new == NULL) return;
    memcpy(&new[ab->len],s,len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab){
    free(ab->b);
}

/*** input ***/

char *editorPrompt(char *prompt, void (*callback)(char *, int)){
    size_t bufsize = 128;
    char *buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';

    while(1){
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey();

        switch(c){
            case '\x1b': //escape, cancel prompt
                editorSetStatusMessage("");
                if(callback) callback(buf,c);
                free(buf);
                return NULL;
            
            case BACKSPACE:
            case CTRL('h'): //old time backspace
                if(buflen != 0) buf[--buflen] = '\0';
                break;
            
            case '\r': //enter, confirm prompt
                if(buflen != 0){
                    editorSetStatusMessage("");
                    if(callback) callback(buf,c);
                    return buf;
                }
                break;
            default:
                if(!iscntrl(c) && c < 128){ //is valid character, add to buffer
                    if(buflen == bufsize -1){ //went above allocated buffer, double it
                        bufsize *= 2;
                        buf = realloc(buf, bufsize);
                    }
                    buf[buflen++] = c;
                    buf[buflen] = '\0';
                }
                break;            
        }
        if(callback) callback(buf,c);
    }
}

void editorMoveCursor(int key){
    erow *row = (E.cy >= E.numrows)? NULL : &E.row[E.cy];

    switch(key){
        case HOME_KEY:
            E.cx = 0;
            break;
        case END_KEY:
            E.cx = row->size -1;
            break;
        case PAGE_UP:
            E.cy = E.rowoffset;
            break;
        case PAGE_DOWN:
            E.cy = E.rowoffset + E.screenrows -1;
            break;
        case ARROW_LEFT:
            if(E.cx != 0) E.cx--;
            else if(E.cy > 0){
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break;
        case ARROW_RIGHT:
            if(row && E.cx < row->size) E.cx++;
            else if(row && E.cx == row->size){
                E.cy++;
                E.cx = 0;
            }
            break;
        case ARROW_UP:
            if(E.cy != 0) E.cy--;
            break;
        case ARROW_DOWN:
            if(E.cy != E.numrows) E.cy++;
            break;
    }

    row = (E.cy >= E.numrows)? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if(E.cx > rowlen) E.cx = rowlen;
}

void editorProcessKeypress(){
    static int quit_times = QUILLO_QUIT_TIMES;
    int c = editorReadKey();

    switch(c){
        //editor operations
        case CTRL_KEY('q'): //ctrl q = quit
        if(E.dirty && quit_times > 0){
            editorSetStatusMessage("WARNING! File has unsaved changes. Press Ctrl Q %d more times to quit",quit_times);
            quit_times--;
            return;
        }
            //clear the screen
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);

            exit(0);
            break;

        case CTRL_KEY('s'): //ctrl s = save
            editorSave();
            break;

        case CTRL_KEY('l'): //refresh screen
        case '\x1b': //escape
            break;

        case CTRL_KEY('f'):
            editorFind();
            break;

        //text operations
        case DELETE_KEY: 
        case BACKSPACE:
        case CTRL_KEY('h'): //old time backspace
        if(c == DELETE_KEY) editorMoveCursor(ARROW_RIGHT);
            editorDelChar();
            break;
        
        case '\r': //enter
            editorInsertNewLine();
            break;

        //cursor movement
        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
        case PAGE_UP:
        case PAGE_DOWN:
        case HOME_KEY:
        case END_KEY:
            editorMoveCursor(c);
            break;
        
        //base case
        default:
            editorInsertChar(c);
            break;
    }
    quit_times = QUILLO_QUIT_TIMES;
}

/*** output ***/
void editorScroll(){
    E.rx = 0;
    if(E.cy < E.numrows){
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }
    //vertical
    if(E.cy < E.rowoffset){ //above the visible window
        E.rowoffset = E.cy;
    }
    if(E.cy >= E.rowoffset + E.screenrows){ //below the visible window
        E.rowoffset = E.cy - E.screenrows+1;
    }
    //horizontal
    if(E.rx < E.coloffset){ //left of the visible window
        E.coloffset = E.rx;
    }
    if(E.rx >= E.coloffset + E.screencols){ //right of the visible window
        E.coloffset = E.rx - E.screencols+1;
    }
}

void editorProcessRow(struct abuf *ab, erow *row, int len){
    int currentColor = -1;
    char *c = &row->render[E.coloffset];
    unsigned char *hl = &row->hl[E.coloffset];

    for(int j=0;j<len;j++){
        //non printable characters
        if(iscntrl(c[j])){
            char sym = (c[j] <= 26) ? '@' + c[j] : '?';
            abAppend(ab, "\x1b[7m",4);
            abAppend(ab,&sym, 1);
            abAppend(ab,"\x1b[m",3);
            if(currentColor != -1){
                char buf[16];
                int clen = snprintf(buf,sizeof(buf),"\x1b[%dm",currentColor);
                abAppend(ab,buf,clen);
            }
            continue;
        }

        if(hl[j] == HL_NORMAL){
            if(currentColor != -1) abAppend(ab, "\x1b[39m", 5);
            abAppend(ab, &c[j], 1);
            currentColor = -1;
            continue;
        }
        int color = editorSyntaxToColor(hl[j]);
        if(color != currentColor){
            currentColor = color;
            char buf[16];
            int clen = snprintf(buf, sizeof(buf), "\x1b[%dm",color);
            abAppend(ab, buf, clen);
        }        
        abAppend(ab, &c[j], 1);
    }    
    abAppend(ab, "\x1b[39m", 5);
}

void editorDrawRows(struct abuf *ab){
    for(int y=0;y<E.screenrows;y++){
        int filerow = y + E.rowoffset;
        if(filerow < E.numrows){
            int len = E.row[filerow].rsize - E.coloffset;
            if(len < 0) len = 0;
            if(len > E.screencols) len = E.screencols;

            editorProcessRow(ab, &E.row[filerow],len);
        }

        if(y == E.screenrows/3 && E.numrows == 0){
            //display welcome message
            char welcome[80];
            int welcomelen = snprintf(welcome, sizeof(welcome),"QUILLO editor -- version %s",QUILLO_VERSION);
            if(welcomelen > E.screencols) welcomelen = E.screencols;

            int padding = (E.screencols - welcomelen)/2;
            if(padding){
                abAppend(ab, "~",1);
                padding--;
            }
            while(padding--) abAppend(ab," ",1);

            abAppend(ab,welcome,welcomelen);
        }
        if(y >= E.numrows+1) abAppend(ab, "~",1);

        abAppend(ab, "\x1b[K",3); //erase rest of line
        abAppend(ab,"\r\n",2); //newline
    }
}

void editorDrawStatusBar(struct abuf *ab){
    abAppend(ab,"\x1b[7m",4); //inverted colors

    char status[80], rstatus[80];
    int len = snprintf(status,sizeof(status),
        "%.30s - %d lines %s",E.filename ? E.filename : "[NO NAME]",E.numrows, E.dirty ? "(modified)":"");
    
    int rlen = snprintf(rstatus,sizeof(rstatus),"%s %d/%d",E.syntax?E.syntax->filetype:"plain text",E.cy+1,E.numrows);
    if(len > E.screencols) len = E.screencols;
    abAppend(ab,status,len);

    while(len < E.screencols){
        if(E.screencols - len == rlen){
            abAppend(ab,rstatus,rlen);
            break;
        }
        abAppend(ab," ",1);
        len++;
    }

    abAppend(ab,"\x1b[m",3); //normal colors
    abAppend(ab,"\r\n",2); //normal colors
}

void editorDrawMessageBar(struct abuf *ab){
    abAppend(ab, "\x1b[K",3);
    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencols) msglen = E.screencols;
    if (msglen && time(NULL) - E.statusmsg_time < QUILLO_MESSAGE_DURATION)
        abAppend(ab, E.statusmsg, msglen);
}

void editorRefreshScreen(){
    editorScroll();

    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6);//hide cursor
    abAppend(&ab, "\x1b[H", 3); //reset cursor

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    //place cursor where it should be
    char buf[32];
    snprintf(buf,sizeof(buf),"\x1b[%d;%dH",(E.cy-E.rowoffset)+1,(E.rx-E.coloffset)+1);
    abAppend(&ab,buf,strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6);//show cursor

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/*** init ***/

void initEditor(){
    E.cx = 0;
    E.cy = 0;
    E.numrows = 0;
    E.row = NULL;
    E.rowoffset = 0;
    E.coloffset = 0;
    E.rx = 0;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;
    if(getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
    E.screenrows -= 2;
    E.dirty = 0;
    E.syntax = NULL;
}

int main(int argc, char *argv[]){
    initEditor();
    enableRawMode();

    if(argc >= 2){
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP: Ctrl-q = quit  Ctrl-s = save  Ctrl-f = find");

    while(1){
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}