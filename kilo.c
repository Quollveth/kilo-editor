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

#define KILO_VERSION "0.0.1"
#define CTRL_KEY(k) ((k) & 0x1f)
#define KILO_TAB_STOP 8
#define KILO_MESSAGE_DURATION 5 //in seconds
#define KILO_QUIT_TIMES 2 //how many times ctrl q has to be pressed before quitting without saving

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

/*** data ***/

typedef struct erow{
    int size;
    int rsize;
    char *chars;
    char *render;
} erow;

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
};

struct editorConfig E;

/*** prototypes ***/

void editorSetStatusMessage(const char *fmt, ...);

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

/*** row operations ***/
int editorRowCxToRx(erow *row, int cx){
    int rx = 0;
    int j;
    for(j=0; j<cx; j++){
        if(row->chars[j] == '\t')
            rx += (KILO_TAB_STOP -1) - (rx % KILO_TAB_STOP);
        rx++;
    }
    return rx;
}

void editorUpdateRow(erow *row){
    int tabs = 0;
    int j;
    for(j=0; j<row->size; j++){
        if(row->chars[j] == '\t') tabs++;
    }

    free(row->render);
    row->render = malloc(row->size + tabs* (KILO_TAB_STOP-1) +1);

    int idx = 0;
    for(j=0; j<row->size; j++){
        if(row->chars[j]!='\t'){
            row->render[idx++] = row->chars[j];
            continue;
        }
        row->render[idx++] = ' ';
        while(idx % KILO_TAB_STOP != 0) row->render[idx++] = ' ';
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

void editorInsertRow(int at,char *s, size_t len){
    if(at < 0 || at > E.numrows) return;
    
    E.row = realloc(E.row, sizeof(erow) * (E.numrows+1));
    memmove(&E.row[at+1], &E.row[at], sizeof(erow)*(E.numrows - at));

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars,s,len);
    E.row[at].chars[len] = '\0';
    E.row[at].render = NULL;
    E.row[at].rsize = 0;

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
}

void editorDelRow(int at){
    if(at < 0 || at >= E.numrows) return;
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at],&E.row[at +1],E.numrows-at);
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
    if(E.filename == NULL) return;

    int len;
    char *buf = editorRowsToString(&len);

    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if(fd == -1) goto error;

    if(ftruncate(fd, len) == -1) goto error;

    if(write(fd, buf, len) != len) goto error;

    close(fd);
    free(buf);
    E.dirty = 0;
    editorSetStatusMessage("%d bytes written to disk",len);
    return;

    error:
    close(fd);
    free(buf);
    editorSetStatusMessage("I/O Error while saving: %s",strerror(errno));
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
    static int quit_times = KILO_QUIT_TIMES;
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
    quit_times = KILO_QUIT_TIMES;
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

void editorDrawRows(struct abuf *ab){
    for(int y=0;y<E.screenrows;y++){
        int filerow = y + E.rowoffset;
        if(filerow < E.numrows){
            int len = E.row[filerow].rsize - E.coloffset;
            if(len < 0) len = 0;
            if(len > E.screencols) len = E.screencols;
            abAppend(ab,&E.row[filerow].render[E.coloffset],len);
        }

        if(y == E.screenrows/3 && E.numrows == 0){
            //display welcome message
            char welcome[80];
            int welcomelen = snprintf(welcome, sizeof(welcome),"Kilo editor -- version %s",KILO_VERSION);
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
    
    int rlen = snprintf(rstatus,sizeof(rstatus),"%d/%d",E.cy+1,E.numrows);
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
    if (msglen && time(NULL) - E.statusmsg_time < KILO_MESSAGE_DURATION)
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
}

int main(int argc, char *argv[]){
    initEditor();
    enableRawMode();

    if(argc >= 2){
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP: Ctrl-q = quit  Ctrl-s = save");

    while(1){
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}