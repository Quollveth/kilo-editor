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

/*** defines ***/

#define KILO_VERSION "0.0.1"
#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKeys {
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
    char *chars;
} erow;

struct editorConfig {
    int cx, cy; //cursor position
    int screenrows, screencols; //screen size
    int rowoffset, coloffset; //scrolling
    int numrows;
    erow *row;    
    struct termios org_termios;
};

struct editorConfig E;

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
void editorAppendRow(char *s, size_t len){
    E.row = realloc(E.row, sizeof(erow) * (E.numrows+1));

    int at = E.numrows;
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars,s,len);
    E.row[at].chars[len] = '\0';
    E.numrows++;
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
        editorAppendRow(line,linelen);
    }    

    free(line);
    fclose(fp);
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
            E.cy = 0;
            break;
        case PAGE_DOWN:
            E.cy = E.numrows -1;
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
    int c = editorReadKey();

    switch(c){
        case CTRL_KEY('q'): //ctrl q = quit
            //clear the screen
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);

            exit(0);
            break;
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
        case DELETE_KEY: break;
        default:
            break;
    }
}

/*** output ***/
void editorScroll(){
    //vertical
    if(E.cy < E.rowoffset){ //above the visible window
        E.rowoffset = E.cy;
    }
    if(E.cy >= E.rowoffset + E.screenrows){ //below the visible window
        E.rowoffset = E.cy - E.screenrows+1;
    }
    //horizontal
    if(E.cx < E.coloffset){ //left of the visible window
        E.coloffset = E.cx;
    }
    if(E.cx >= E.coloffset + E.screencols){ //right of the visible window
        E.coloffset = E.cx - E.screencols+1;
    }
}

void editorDrawRows(struct abuf *ab){
    for(int y=0;y<E.screenrows;y++){
        int filerow = y + E.rowoffset;
        if(filerow < E.numrows){
            int len = E.row[filerow].size - E.coloffset;
            if(len < 0) len = 0;
            if(len > E.screencols) len = E.screencols;
            abAppend(ab,&E.row[filerow].chars[E.coloffset],len);
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
        //do not newline on the last line
        if(y < E.screenrows-1) abAppend(ab,"\r\n",2);
    }
}

void editorRefreshScreen(){
    editorScroll();

    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6);//hide cursor
    abAppend(&ab, "\x1b[H", 3); //reset cursor

    editorDrawRows(&ab);

    //place cursor where it should be
    char buf[32];
    snprintf(buf,sizeof(buf),"\x1b[%d;%dH",(E.cy-E.rowoffset)+1,(E.cx-E.coloffset)+1);
    abAppend(&ab,buf,strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6);//show cursor

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

/*** init ***/

void initEditor(){
    E.cx = 0;
    E.cy = 0;
    E.numrows = 0;
    E.row = NULL;
    E.rowoffset = 0;
    E.coloffset = 0;
    if(getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

int main(int argc, char *argv[]){
    initEditor();
    enableRawMode();
    if(argc >= 2){
        editorOpen(argv[1]);
    }
    while(1){
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}