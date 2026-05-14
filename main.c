/*
 * CookieOS Customizer — X11 + Cairo, single file
 * Tabs: GRUB | Wallpaper | Login Screen
 */
#define _GNU_SOURCE
#include <math.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>

/* ── window ────────────────────────────────────────── */
#define WIN_W  1080
#define WIN_H   720
#define HDR_H    54
#define NAV_W   182
#define SB_H     26
#define CON_Y   HDR_H
#define CON_H  (WIN_H - HDR_H - SB_H)

/* ── color helper ──────────────────────────────────── */
typedef struct { double r,g,b; } Col;
static Col col(int r,int g,int b){ Col c={(r)/255.0,(g)/255.0,(b)/255.0}; return c; }

/* named colors */
static Col BG,SIDEBAR,HEADER,BORDER_C,ACCENT_C,TEXT_C,TEXT2,TEXT3;
static Col GREEN_C,ORANGE_C,BTN_C,BTN_OK_C,BTN_PRP_C,INPUT_BG_C,SEL_C,RED_C,WHITE_C;

static void init_colors(void){
    BG=col(26,30,46); SIDEBAR=col(18,22,42); HEADER=col(16,21,42);
    BORDER_C=col(40,48,90); ACCENT_C=col(74,120,224);
    TEXT_C=col(208,216,240); TEXT2=col(120,136,184); TEXT3=col(80,96,144);
    GREEN_C=col(76,175,138); ORANGE_C=col(232,168,56);
    BTN_C=col(34,44,80); BTN_OK_C=col(30,92,56); BTN_PRP_C=col(74,30,128);
    INPUT_BG_C=col(13,17,32); SEL_C=col(35,48,104);
    RED_C=col(224,85,85); WHITE_C=col(255,255,255);
}

/* ── state ─────────────────────────────────────────── */
typedef enum { TAB_GRUB=0, TAB_WALL=1, TAB_LOGIN=2 } Tab;
typedef enum { DE_KDE,DE_GNOME,DE_XFCE,DE_UNKNOWN } DE;
typedef enum { DM_SDDM,DM_LIGHTDM,DM_GDM,DM_UNKNOWN } DM;

#define MAX_THEMES 64
#define MAX_WALLS 128
#define MAX_SDDM  32

typedef struct { char path[512]; char name[128]; } Entry;

typedef struct {
    Tab tab; DE de; DM dm;
    int is_root;
    char username[64];
    char home[256];

    /* GRUB */
    int  grub_timeout;
    char grub_default[64];
    char grub_resolution[32];
    char grub_cmdline[256];
    char grub_background[512];
    Entry grub_themes[MAX_THEMES];
    int  grub_theme_count, grub_theme_sel;
    char grub_status[256];
    int  grub_ok;

    /* Wallpaper */
    char wall_dir[512];
    char wall_paths[MAX_WALLS][512];
    char wall_names[MAX_WALLS][128];
    int  wall_count, wall_sel;
    char wall_status[256];
    int  wall_ok;

    /* Login */
    char  login_bg[512];
    char  login_status[256];
    int   login_ok;
    Entry sddm_themes[MAX_SDDM];
    int   sddm_count, sddm_sel;
    char  ldm_greeter[64];

    /* input */
    int  focused; /* widget id */
} St;

typedef struct {
    Display *dpy;
    Window   win;
    cairo_surface_t *surf;
    cairo_t *cr;
    St s;
    int need_redraw;
    int scroll[3];
    int mx,my;
} App;

/* ── buttons registry ──────────────────────────────── */
typedef struct { int x,y,w,h,id; } Btn;
#define MAX_BTNS 128
static Btn g_btns[MAX_BTNS];
static int g_nb=0;
static void btn_clear(void){ g_nb=0; }
static void btn_add(int x,int y,int w,int h,int id){
    if(g_nb<MAX_BTNS){ g_btns[g_nb++]=(Btn){x,y,w,h,id}; }
}
static int in_btn(int mx,int my,int x,int y,int w,int h){
    return mx>=x&&mx<=x+w&&my>=y&&my<=y+h;
}

/* ── utils ─────────────────────────────────────────── */
static int run_cmd(const char *cmd){ return system(cmd)==0; }

static void read_file(const char *p,char *buf,size_t sz){
    FILE *f=fopen(p,"r"); if(!f)return;
    size_t n=fread(buf,1,sz-1,f); buf[n]=0; fclose(f);
}
static int write_file(const char *p,const char *c){
    FILE *f=fopen(p,"w"); if(!f)return 0;
    fputs(c,f); fclose(f); return 1;
}

static void grub_get(const char *content,const char *key,char *out,size_t sz){
    char pat[128]; snprintf(pat,sizeof(pat),"%s=",key);
    const char *p=strstr(content,pat);
    if(!p){out[0]=0;return;}
    p+=strlen(pat);
    if(*p=='"')p++;
    size_t i=0;
    while(*p&&*p!='"'&&*p!='\n'&&i<sz-1)out[i++]=*p++;
    out[i]=0;
}

static void grub_set(char *content,size_t cap,const char *key,const char *val){
    char pat[128]; snprintf(pat,sizeof(pat),"%s=",key);
    char nl[1024]; snprintf(nl,sizeof(nl),"%s=\"%s\"",key,val);
    char *p=strstr(content,pat);
    if(p){
        char *eol=strchr(p,'\n');
        size_t before=p-content, after=eol?strlen(eol):0, nll=strlen(nl);
        if(before+nll+after+1<cap){
            memmove(p+nll,eol?eol:p+strlen(p),after+1);
            memcpy(p,nl,nll);
        }
    } else {
        strncat(content,"\n",cap-strlen(content)-1);
        strncat(content,nl,cap-strlen(content)-1);
        strncat(content,"\n",cap-strlen(content)-1);
    }
}


/* ── portable file/dir dialog via zenity or kdialog ─── */
static int pick_path(const char *title, int dir_only, char *out, size_t outsz) {
    char cmd[512];
    FILE *fp;

    /* try zenity */
    if(system("which zenity >/dev/null 2>&1")==0) {
        if(dir_only)
            snprintf(cmd,sizeof(cmd),"zenity --file-selection --directory --title='%s' 2>/dev/null",title);
        else
            snprintf(cmd,sizeof(cmd),"zenity --file-selection --title='%s' "
                "--file-filter='Images | *.png *.jpg *.jpeg *.bmp *.webp' 2>/dev/null",title);
        fp=popen(cmd,"r");
        if(fp){ size_t n=fread(out,1,outsz-1,fp); pclose(fp);
                while(n>0&&(out[n-1]=='\n'||out[n-1]=='\r'))n--;
                out[n]=0; return n>0; }
    }
    /* try kdialog */
    if(system("which kdialog >/dev/null 2>&1")==0) {
        if(dir_only)
            snprintf(cmd,sizeof(cmd),"kdialog --getexistingdirectory / --title '%s' 2>/dev/null",title);
        else
            snprintf(cmd,sizeof(cmd),"kdialog --getopenfilename / '*.png *.jpg *.jpeg *.bmp *.webp' --title '%s' 2>/dev/null",title);
        fp=popen(cmd,"r");
        if(fp){ size_t n=fread(out,1,outsz-1,fp); pclose(fp);
                while(n>0&&(out[n-1]=='\n'||out[n-1]=='\r'))n--;
                out[n]=0; return n>0; }
    }
    /* try xdg-open based approach - just open file manager */
    /* last resort: prompt user to type in the input box */
    return 0;
}

/* ── add a grub theme dir entry ─────────────────────── */
static void add_grub_theme_dir(St *s, const char *dir) {
    if(!dir||!dir[0]) return;
    /* check theme.txt exists */
    char tp[600]; snprintf(tp,sizeof(tp),"%s/theme.txt",dir);
    if(access(tp,F_OK)!=0) {
        /* maybe user picked parent - try one level down */
        int found=0;
        DIR *d=opendir(dir);
        if(d){ struct dirent *e;
            while((e=readdir(d))&&!found){
                if(e->d_name[0]=='.')continue;
                char sub[600]; snprintf(sub,sizeof(sub),"%s/%s/theme.txt",dir,e->d_name);
                if(access(sub,F_OK)==0){
                    snprintf(tp,sizeof(tp),"%s/%s",dir,e->d_name);
                    dir=tp; found=1;
                }
            }
            closedir(d);
        }
        if(!found){ s->grub_status[0]=0;
            strcpy(s->grub_status,"Directory does not contain theme.txt"); s->grub_ok=0; return; }
    }
    /* check not already in list */
    for(int i=0;i<s->grub_theme_count;i++)
        if(strcmp(s->grub_themes[i].path,dir)==0){ s->grub_theme_sel=i; return; }
    if(s->grub_theme_count>=MAX_THEMES) return;
    strncpy(s->grub_themes[s->grub_theme_count].path,dir,511);
    /* name = last component */
    const char *slash=strrchr(dir,'/');
    strncpy(s->grub_themes[s->grub_theme_count].name,slash?slash+1:dir,127);
    s->grub_theme_sel=s->grub_theme_count;
    s->grub_theme_count++;
    snprintf(s->grub_status,sizeof(s->grub_status),"Added theme: %s",
             s->grub_themes[s->grub_theme_sel].name);
    s->grub_ok=1;
}

/* ── system detect ─────────────────────────────────── */
static void detect(St *s){
    s->is_root=(geteuid()==0);
    const char *su=getenv("SUDO_USER");
    if(su&&*su) strncpy(s->username,su,63);
    else{ struct passwd *pw=getpwuid(getuid());
          if(pw)strncpy(s->username,pw->pw_name,63); }
    { struct passwd *pw=getpwnam(s->username);
      strncpy(s->home,pw?pw->pw_dir:(getenv("HOME")?getenv("HOME"):"/root"),255); }

    const char *xdg=getenv("XDG_CURRENT_DESKTOP");
    const char *dss=getenv("DESKTOP_SESSION");
    s->de=DE_UNKNOWN;
    if((xdg&&(strstr(xdg,"KDE")||strstr(xdg,"kde")))||
       (dss&&(strstr(dss,"plasma")||strstr(dss,"kde")))) s->de=DE_KDE;
    else if((xdg&&strstr(xdg,"GNOME"))||(dss&&strstr(dss,"gnome"))) s->de=DE_GNOME;
    else if((xdg&&strstr(xdg,"XFCE"))||(dss&&strstr(dss,"xfce")))  s->de=DE_XFCE;

    s->dm=DM_UNKNOWN;
    if(system("systemctl is-active --quiet sddm 2>/dev/null")==0)    s->dm=DM_SDDM;
    else if(system("systemctl is-active --quiet lightdm 2>/dev/null")==0) s->dm=DM_LIGHTDM;
    else if(system("systemctl is-active --quiet gdm 2>/dev/null")==0 ||
            system("systemctl is-active --quiet gdm3 2>/dev/null")==0) s->dm=DM_GDM;
    else if(access("/etc/sddm.conf",F_OK)==0)              s->dm=DM_SDDM;
    else if(access("/etc/lightdm/lightdm.conf",F_OK)==0)  s->dm=DM_LIGHTDM;
    else if(access("/etc/gdm3/custom.conf",F_OK)==0)       s->dm=DM_GDM;
}

static void load_grub(St *s){
    static char buf[16384]; buf[0]=0;
    read_file("/etc/default/grub",buf,sizeof(buf));
    char tmp[64];
    grub_get(buf,"GRUB_TIMEOUT",tmp,sizeof(tmp));
    s->grub_timeout=tmp[0]?atoi(tmp):5;
    grub_get(buf,"GRUB_DEFAULT",s->grub_default,sizeof(s->grub_default));
    grub_get(buf,"GRUB_GFXMODE",s->grub_resolution,sizeof(s->grub_resolution));
    if(!s->grub_resolution[0])strcpy(s->grub_resolution,"auto");
    grub_get(buf,"GRUB_CMDLINE_LINUX_DEFAULT",s->grub_cmdline,sizeof(s->grub_cmdline));
    if(!s->grub_cmdline[0])strcpy(s->grub_cmdline,"quiet splash");
    grub_get(buf,"GRUB_BACKGROUND",s->grub_background,sizeof(s->grub_background));
}

static void scan_dir_entries(const char *base,Entry *arr,int *cnt,int max){
    DIR *d=opendir(base); if(!d)return;
    struct dirent *e;
    while((e=readdir(d))&&*cnt<max){
        if(e->d_name[0]=='.')continue;
        char path[520]; snprintf(path,sizeof(path),"%s/%s/theme.txt",base,e->d_name);
        if(access(path,F_OK)==0){
            snprintf(arr[*cnt].path,512,"%s/%s",base,e->d_name);
            strncpy(arr[*cnt].name,e->d_name,127);
            (*cnt)++;
        }
    }
    closedir(d);
}

static void scan_grub_themes(St *s){
    s->grub_theme_count=1;
    strcpy(s->grub_themes[0].path,"");
    strcpy(s->grub_themes[0].name,"(brak motywu)");
    scan_dir_entries("/usr/share/grub/themes",s->grub_themes,&s->grub_theme_count,MAX_THEMES);
    scan_dir_entries("/boot/grub/themes",s->grub_themes,&s->grub_theme_count,MAX_THEMES);
    scan_dir_entries("/usr/share/grub2/themes",s->grub_themes,&s->grub_theme_count,MAX_THEMES);
    s->grub_theme_sel=0;
}

static int is_img(const char *name){
    const char *exts[]={"jpg","jpeg","png","bmp","webp","tiff","tga",NULL};
    const char *dot=strrchr(name,'.');
    if(!dot)return 0;
    for(int i=0;exts[i];i++) if(strcasecmp(dot+1,exts[i])==0)return 1;
    return 0;
}

static void scan_walls_dir(St *s,const char *dir,int depth){
    if(depth<0||s->wall_count>=MAX_WALLS)return;
    DIR *d=opendir(dir); if(!d)return;
    struct dirent *e;
    while((e=readdir(d))&&s->wall_count<MAX_WALLS){
        if(e->d_name[0]=='.')continue;
        char path[768];
        snprintf(path,sizeof(path),"%s/%s",dir,e->d_name);
        struct stat st;
        if(stat(path,&st)!=0)continue;
        if(S_ISDIR(st.st_mode)){
            scan_walls_dir(s,path,depth-1);
        } else if(S_ISREG(st.st_mode)&&is_img(e->d_name)){
            strncpy(s->wall_paths[s->wall_count],path,511);
            s->wall_paths[s->wall_count][511]=0;
            /* build display name: subdir/filename */
            const char *rel=path+strlen(s->wall_dir);
            if(*rel=='/')rel++;
            strncpy(s->wall_names[s->wall_count],rel,127);
            s->wall_names[s->wall_count][127]=0;
            s->wall_count++;
        }
    }
    closedir(d);
}

static void scan_walls(St *s,const char *dir){
    s->wall_count=0;
    s->wall_sel=-1;
    strncpy(s->wall_dir,dir,511);
    s->wall_dir[511]=0;
    scan_walls_dir(s,dir,3); /* scan up to 3 levels deep */
}

static void scan_sddm(St *s){
    s->sddm_count=1;
    strcpy(s->sddm_themes[0].path,"");
    strcpy(s->sddm_themes[0].name,"(default)");
    s->sddm_sel=0;
    DIR *d=opendir("/usr/share/sddm/themes"); if(!d)return;
    struct dirent *e;
    while((e=readdir(d))&&s->sddm_count<MAX_SDDM){
        if(e->d_name[0]=='.')continue;
        char p[520]; snprintf(p,sizeof(p),"/usr/share/sddm/themes/%s",e->d_name);
        struct stat st; stat(p,&st);
        if(S_ISDIR(st.st_mode)){
            strncpy(s->sddm_themes[s->sddm_count].path,p,511);
            strncpy(s->sddm_themes[s->sddm_count].name,e->d_name,127);
            s->sddm_count++;
        }
    }
    closedir(d);
}

/* ── apply ─────────────────────────────────────────── */
static void do_apply_grub(St *s){
    if(!s->is_root){
        strcpy(s->grub_status,"Root required: sudo cookieos-customizer");
        s->grub_ok=0; return;
    }
    static char buf[16384]; buf[0]=0;
    read_file("/etc/default/grub",buf,sizeof(buf));
    char tmp[16]; snprintf(tmp,sizeof(tmp),"%d",s->grub_timeout);
    grub_set(buf,sizeof(buf),"GRUB_TIMEOUT",tmp);
    grub_set(buf,sizeof(buf),"GRUB_DEFAULT",s->grub_default);
    grub_set(buf,sizeof(buf),"GRUB_GFXMODE",s->grub_resolution);
    grub_set(buf,sizeof(buf),"GRUB_CMDLINE_LINUX_DEFAULT",s->grub_cmdline);
    if(s->grub_background[0]) grub_set(buf,sizeof(buf),"GRUB_BACKGROUND",s->grub_background);
    if(s->grub_theme_sel>0&&s->grub_theme_sel<s->grub_theme_count){
        char tp[600]; snprintf(tp,sizeof(tp),"%s/theme.txt",s->grub_themes[s->grub_theme_sel].path);
        grub_set(buf,sizeof(buf),"GRUB_THEME",tp);
    }
    if(!write_file("/etc/default/grub",buf)){
        strcpy(s->grub_status,"Error writing /etc/default/grub"); s->grub_ok=0; return;
    }
    int ok=system("update-grub 2>/dev/null")==0 ||
           system("grub-mkconfig -o /boot/grub/grub.cfg 2>/dev/null")==0;
    if(ok){ strcpy(s->grub_status,"Gotowe! GRUB zaktualizowany."); s->grub_ok=1; }
    else  { strcpy(s->grub_status,"Zapisano. Uruchom recznie: sudo update-grub"); s->grub_ok=0; }
}

static void do_apply_wall(St *s){
    if(s->wall_sel<0||s->wall_sel>=s->wall_count){
        strcpy(s->wall_status,"Wybierz tapete z listy"); s->wall_ok=0; return;
    }
    const char *path=s->wall_paths[s->wall_sel];
    char cmd[1100]; int ok=0;
    switch(s->de){
        case DE_KDE:
            snprintf(cmd,sizeof(cmd),
                "qdbus org.kde.plasmashell /PlasmaShell "
                "org.kde.PlasmaShell.evaluateScript "
                "\"var d=desktops();for(var i=0;i<d.length;i++){"
                "d[i].wallpaperPlugin='org.kde.image';"
                "d[i].currentConfigGroup=['Wallpaper','org.kde.image','General'];"
                "d[i].writeConfig('Image','file://%s');}\" 2>/dev/null",path);
            ok=run_cmd(cmd);
            if(!ok){ snprintf(cmd,sizeof(cmd),"plasma-apply-wallpaperimage '%s' 2>/dev/null",path); ok=run_cmd(cmd); }
            break;
        case DE_GNOME:
            snprintf(cmd,sizeof(cmd),"gsettings set org.gnome.desktop.background picture-uri 'file://%s' 2>/dev/null",path);
            ok=run_cmd(cmd);
            snprintf(cmd,sizeof(cmd),"gsettings set org.gnome.desktop.background picture-uri-dark 'file://%s' 2>/dev/null",path);
            run_cmd(cmd);
            break;
        case DE_XFCE:
            snprintf(cmd,sizeof(cmd),
                "xfconf-query -c xfce4-desktop -p /backdrop/screen0/monitor0/workspace0/last-image -s '%s' 2>/dev/null",path);
            ok=run_cmd(cmd);
            break;
        default:
            strcpy(s->wall_status,"Nieznane DE — ustaw recznie"); s->wall_ok=0; return;
    }
    if(ok){ strcpy(s->wall_status,"Wallpaper set!"); s->wall_ok=1; }
    else  { strcpy(s->wall_status,"Error setting wallpaper"); s->wall_ok=0; }
}

static void do_apply_login(St *s){
    if(!s->is_root){
        strcpy(s->login_status,"Root required: sudo cookieos-customizer");
        s->login_ok=0; return;
    }
    int ok=0;
    static char buf[8192];
    switch(s->dm){
        case DM_SDDM:
            buf[0]=0;
            read_file("/etc/sddm.conf",buf,sizeof(buf));
            if(s->sddm_sel>0) grub_set(buf,sizeof(buf),"Current",s->sddm_themes[s->sddm_sel].name);
            if(s->login_bg[0]) grub_set(buf,sizeof(buf),"Background",s->login_bg);
            ok=write_file("/etc/sddm.conf",buf);
            break;
        case DM_LIGHTDM: {
            char content[1024];
            snprintf(content,sizeof(content),"[Greeter]\nbackground=%s\n",s->login_bg);
            ok=write_file("/etc/lightdm/lightdm-gtk-greeter.conf",content);
            break; }
        case DM_GDM: {
            char css[1024];
            snprintf(css,sizeof(css),"#lockDialogGroup{background:url('%s') center/cover;}\n",s->login_bg);
            mkdir("/etc/gdm3",0755);
            ok=write_file("/etc/gdm3/greeter-background.css",css);
            break; }
        default:
            strcpy(s->login_status,"Unknown DM"); s->login_ok=0; return;
    }
    if(ok){ strcpy(s->login_status,"Saved! Changes visible after reboot."); s->login_ok=1; }
    else  { strcpy(s->login_status,"Error writing konfiguracji"); s->login_ok=0; }
}

/* ── drawing primitives ────────────────────────────── */
static void src(cairo_t *cr, Col c){ cairo_set_source_rgb(cr,c.r,c.g,c.b); }

static void rrect(cairo_t *cr,double x,double y,double w,double h,double r){
    if(r<=0){ cairo_rectangle(cr,x,y,w,h); return; }
    cairo_new_path(cr);
    cairo_arc(cr,x+r,  y+r,  r,M_PI,    3*M_PI/2);
    cairo_arc(cr,x+w-r,y+r,  r,3*M_PI/2,0);
    cairo_arc(cr,x+w-r,y+h-r,r,0,       M_PI/2);
    cairo_arc(cr,x+r,  y+h-r,r,M_PI/2,  M_PI);
    cairo_close_path(cr);
}

static void fill_rect(cairo_t *cr,double x,double y,double w,double h,double r,Col c){
    src(cr,c); rrect(cr,x,y,w,h,r); cairo_fill(cr);
}
static void stroke_rect(cairo_t *cr,double x,double y,double w,double h,double r,Col c,double lw){
    src(cr,c); cairo_set_line_width(cr,lw); rrect(cr,x,y,w,h,r); cairo_stroke(cr);
}
static void line(cairo_t *cr,double x1,double y1,double x2,double y2,Col c,double lw){
    src(cr,c); cairo_set_line_width(cr,lw);
    cairo_move_to(cr,x1,y1); cairo_line_to(cr,x2,y2); cairo_stroke(cr);
}

static void txt(cairo_t *cr,const char *s,double x,double y,Col c,double sz,int bold){
    src(cr,c);
    cairo_select_font_face(cr,"DejaVu Sans",CAIRO_FONT_SLANT_NORMAL,
        bold?CAIRO_FONT_WEIGHT_BOLD:CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr,sz);
    cairo_move_to(cr,x,y);
    cairo_show_text(cr,s);
}

static double txtw(cairo_t *cr,const char *s,double sz,int bold){
    cairo_select_font_face(cr,"DejaVu Sans",CAIRO_FONT_SLANT_NORMAL,
        bold?CAIRO_FONT_WEIGHT_BOLD:CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr,sz);
    cairo_text_extents_t e; cairo_text_extents(cr,s,&e);
    return e.width;
}

static Col col_hover(Col c){ return col((int)(c.r*255+20),(int)(c.g*255+20),(int)(c.b*255+20)); }

static void draw_btn(cairo_t *cr,int x,int y,int w,int h,
                     const char *label,Col bg,int hover,int id){
    Col c=hover?col_hover(bg):bg;
    fill_rect(cr,x,y,w,h,5,c);
    stroke_rect(cr,x,y,w,h,5,col_hover(c),1);
    double tw=txtw(cr,label,12,1);
    txt(cr,label,x+(w-tw)/2,y+h/2+5,WHITE_C,12,1);
    btn_add(x,y,w,h,id);
}

static void draw_input(cairo_t *cr,int x,int y,int w,int h,
                       const char *val,int focused,int id){
    fill_rect(cr,x,y,w,h,5,INPUT_BG_C);
    stroke_rect(cr,x,y,w,h,5,focused?ACCENT_C:BORDER_C,focused?1.5:1.0);
    cairo_save(cr);
    cairo_rectangle(cr,x+6,y,w-12,h); cairo_clip(cr);
    txt(cr,val,x+8,y+h/2+5,TEXT_C,12,0);
    if(focused){
        double tw=txtw(cr,val,12,0);
        src(cr,TEXT_C); cairo_set_line_width(cr,1.5);
        cairo_move_to(cr,x+8+tw+1,y+4); cairo_line_to(cr,x+8+tw+1,y+h-4);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
    btn_add(x,y,w,h,id);
}

static void draw_group(cairo_t *cr,int x,int y,int w,int h,const char *title){
    fill_rect(cr,x,y,w,h,7,INPUT_BG_C);
    stroke_rect(cr,x,y,w,h,7,BORDER_C,1.0);
    double tw=txtw(cr,title,11,0);
    fill_rect(cr,x+12,y-9,tw+14,18,0,INPUT_BG_C);
    txt(cr,title,x+18,y+1,col(90,120,190),11,0);
}

static void draw_check(cairo_t *cr,int x,int y,const char *label,int checked,int id){
    fill_rect(cr,x,y,16,16,3,INPUT_BG_C);
    stroke_rect(cr,x,y,16,16,3,BORDER_C,1.0);
    if(checked){
        fill_rect(cr,x+2,y+2,12,12,2,ACCENT_C);
        src(cr,WHITE_C); cairo_set_line_width(cr,2);
        cairo_move_to(cr,x+3,y+8); cairo_line_to(cr,x+6,y+12);
        cairo_line_to(cr,x+13,y+4); cairo_stroke(cr);
    }
    txt(cr,label,x+22,y+13,TEXT2,12,0);
    btn_add(x,y,(int)(txtw(cr,label,12,0))+30,16,id);
}

/* ── GRUB page ─────────────────────────────────────── */
static int chk_hidden=0, chk_osprober=0;

static void draw_grub(App *a){
    cairo_t *cr=a->cr; St *s=&a->s;
    int cx=NAV_W+4, cy=CON_Y, cw=WIN_W-NAV_W-8;
    int y=cy+14;

    if(!s->is_root){
        fill_rect(cr,cx,y,cw,30,5,col(42,32,0));
        txt(cr,"Run as root to save GRUB changes (sudo cookieos-customizer)"
,
            cx+10,y+20,ORANGE_C,12,0);
        y+=42;
    }

    /* Boot options */
    draw_group(cr,cx,y,cw,152,"Boot options");
    /* timeout */
    txt(cr,"Timeout:",cx+12,y+30,TEXT2,12,0);
    char tstr[16]; snprintf(tstr,sizeof(tstr),"%d sec",s->grub_timeout);
    draw_input(cr,cx+180,y+16,80,26,tstr,0,0);
    int hm=in_btn(a->mx,a->my,cx+264,y+16,26,26);
    int hp=in_btn(a->mx,a->my,cx+293,y+16,26,26);
    draw_btn(cr,cx+264,y+16,26,26,"-",BTN_C,hm,101);
    draw_btn(cr,cx+293,y+16,26,26,"+",BTN_C,hp,102);

    draw_check(cr,cx+12,y+54,"Hide GRUB menu (hold Shift to show)",chk_hidden,103);

    txt(cr,"Default entry:",cx+12,y+86,TEXT2,12,0);
    draw_input(cr,cx+180,y+72,cw-200,26,s->grub_default,s->focused==104,104);

    txt(cr,"Resolution:",cx+12,y+120,TEXT2,12,0);
    draw_input(cr,cx+180,y+106,cw-200,26,s->grub_resolution,s->focused==105,105);
    y+=166;

    /* Theme */
    draw_group(cr,cx,y,cw,220,"GRUB theme");
    int hbr=in_btn(a->mx,a->my,cx+12,y+14,190,26);
    int hrf=in_btn(a->mx,a->my,cx+208,y+14,110,26);
    draw_btn(cr,cx+12,y+14,190,26,"+ Add theme directory",BTN_C,hbr,201);
    draw_btn(cr,cx+208,y+14,110,26,"Refresh list",BTN_C,hrf,202);

    /* theme list */
    int lx=cx+12,ly=y+48,lw=200,lh=162;
    fill_rect(cr,lx,ly,lw,lh,6,col(10,12,22));
    stroke_rect(cr,lx,ly,lw,lh,6,BORDER_C,1.0);
    cairo_save(cr);
    cairo_rectangle(cr,lx,ly,lw,lh); cairo_clip(cr);
    for(int i=0;i<s->grub_theme_count&&i<7;i++){
        int iy=ly+i*28;
        if(i==s->grub_theme_sel) fill_rect(cr,lx,iy,lw,28,0,SEL_C);
        Col tc=i==s->grub_theme_sel?col(160,200,255):TEXT2;
        txt(cr,s->grub_themes[i].name,lx+10,iy+19,tc,12,i==s->grub_theme_sel);
        btn_add(lx,iy,lw,28,300+i);
    }
    cairo_restore(cr);

    /* preview */
    int px=cx+220,pw=cw-228,ph=162;
    fill_rect(cr,px,ly,pw,ph,6,col(8,10,20));
    stroke_rect(cr,px,ly,pw,ph,6,BORDER_C,1.0);
    if(s->grub_theme_sel>0&&s->grub_theme_sel<s->grub_theme_count){
        char info[300];
        snprintf(info,sizeof(info),"Motyw: %s",s->grub_themes[s->grub_theme_sel].name);
        txt(cr,info,px+12,ly+ph/2,TEXT2,13,1);
        txt(cr,s->grub_themes[s->grub_theme_sel].path,px+12,ly+ph/2+20,TEXT3,10,0);
    } else {
        txt(cr,"Wybierz motyw z listy po lewej",px+12,ly+ph/2,TEXT3,12,0);
    }
    y+=234;

    /* Background */
    draw_group(cr,cx,y,cw,50,"GRUB background (GRUB_BACKGROUND)");
    int bw=cw-140;
    draw_input(cr,cx+12,y+14,bw,26,s->grub_background,s->focused==106,106);
    int hbg=in_btn(a->mx,a->my,cx+bw+16,y+14,110,26);
    draw_btn(cr,cx+bw+16,y+14,110,26,"Browse...",BTN_C,hbg,203);
    y+=64;

    /* Cmdline */
    draw_group(cr,cx,y,cw,80,"Parametry kernela");
    txt(cr,"GRUB_CMDLINE_LINUX_DEFAULT:",cx+12,y+28,TEXT2,12,0);
    draw_input(cr,cx+240,y+14,cw-252,26,s->grub_cmdline,s->focused==107,107);
    draw_check(cr,cx+12,y+52,"Wylacz wykrywanie innych systemow (GRUB_DISABLE_OS_PROBER)",chk_osprober,108);
    y+=94;

    /* Status + Apply */
    if(s->grub_status[0]){
        Col sc=s->grub_ok?GREEN_C:ORANGE_C;
        txt(cr,s->grub_status,cx+10,CON_Y+CON_H-38,sc,12,0);
    }
    int hap=in_btn(a->mx,a->my,cx+cw-260,CON_Y+CON_H-54,248,38);
    draw_btn(cr,cx+cw-260,CON_Y+CON_H-54,248,38,"Zastosuj i uruchom update-grub",BTN_OK_C,hap,204);
}

/* ── Wallpaper page ────────────────────────────────── */
static void draw_wall(App *a){
    cairo_t *cr=a->cr; St *s=&a->s;
    int cx=NAV_W+4, cy=CON_Y, cw=WIN_W-NAV_W-8;
    int y=cy+14;

    const char *dename=s->de==DE_KDE?"KDE Plasma":s->de==DE_GNOME?"GNOME":
                       s->de==DE_XFCE?"XFCE":"Nieznane";
    char info[80]; snprintf(info,sizeof(info),"Environment: %s",dename);
    txt(cr,info,cx+10,y+14,TEXT2,12,0); y+=32;

    /* Dir row */
    draw_group(cr,cx,y,cw,50,"Wallpaper directory");
    int dw=cw-140;
    draw_input(cr,cx+12,y+14,dw,26,s->wall_dir,s->focused==501,501);
    int hdd=in_btn(a->mx,a->my,cx+dw+16,y+14,110,26);
    draw_btn(cr,cx+dw+16,y+14,110,26,"Browse...",BTN_C,hdd,502);
    y+=64;

    /* List + preview */
    int list_h=CON_H-y+cy-80;
    if(list_h<120)list_h=120;
    int lw2=220;

    fill_rect(cr,cx,y,lw2,list_h,6,col(10,12,22));
    stroke_rect(cr,cx,y,lw2,list_h,6,BORDER_C,1.0);
    cairo_save(cr);
    cairo_rectangle(cr,cx,y,lw2,list_h); cairo_clip(cr);
    int vis=list_h/22, off=a->scroll[TAB_WALL];
    for(int i=off;i<s->wall_count&&i<off+vis;i++){
        int iy=y+(i-off)*22;
        if(i==s->wall_sel) fill_rect(cr,cx,iy,lw2,22,0,SEL_C);
        Col tc=i==s->wall_sel?col(160,200,255):TEXT2;
        /* clip name */
        char name[38]; strncpy(name,s->wall_names[i],37); name[37]=0;
        txt(cr,name,cx+8,iy+16,tc,11,0);
        btn_add(cx,iy,lw2,22,600+i);
    }
    cairo_restore(cr);

    /* preview panel */
    int px2=cx+lw2+8, pw2=cw-lw2-12, ph2=list_h-60;
    if(ph2<80)ph2=80;
    fill_rect(cr,px2,y,pw2,ph2,7,col(8,10,20));
    stroke_rect(cr,px2,y,pw2,ph2,7,BORDER_C,1.0);
    if(s->wall_sel>=0&&s->wall_sel<s->wall_count){
        char short_name[60]; strncpy(short_name,s->wall_names[s->wall_sel],59); short_name[59]=0;
        txt(cr,short_name,px2+12,y+ph2/2,TEXT2,13,1);
        txt(cr,s->wall_paths[s->wall_sel],px2+12,y+ph2/2+20,TEXT3,10,0);
    } else {
        txt(cr,"Select a wallpaper from the list on the left",px2+12,y+ph2/2,TEXT3,12,0);
    }
    /* path input below preview */
    draw_input(cr,px2,y+ph2+8,pw2,26,
        s->wall_sel>=0?s->wall_paths[s->wall_sel]:"",0,0);

    /* status + apply */
    if(s->wall_status[0]){
        Col sc=s->wall_ok?GREEN_C:RED_C;
        txt(cr,s->wall_status,cx+10,CON_Y+CON_H-38,sc,12,0);
    }
    int hap=in_btn(a->mx,a->my,cx+cw-210,CON_Y+CON_H-54,198,38);
    draw_btn(cr,cx+cw-210,CON_Y+CON_H-54,198,38,"Set as wallpaper",BTN_C,in_btn(a->mx,a->my,cx+cw-210,CON_Y+CON_H-54,198,38),503);
}

/* ── Login page ────────────────────────────────────── */
static void draw_login(App *a){
    cairo_t *cr=a->cr; St *s=&a->s;
    int cx=NAV_W+4, cy=CON_Y, cw=WIN_W-NAV_W-8;
    int y=cy+14;

    const char *dmname=s->dm==DM_SDDM?"SDDM":s->dm==DM_LIGHTDM?"LightDM":
                       s->dm==DM_GDM?"GDM":"Unknown";
    char info[80]; snprintf(info,sizeof(info),"Login Manager: %s",dmname);
    txt(cr,info,cx+10,y+14,TEXT2,12,0); y+=32;

    if(!s->is_root){
        fill_rect(cr,cx,y,cw,30,5,col(42,32,0));
        txt(cr,"Root required to save DM configuration (sudo cookieos-customizer)",
            cx+10,y+20,ORANGE_C,12,0);
        y+=42;
    }

    /* BG */
    draw_group(cr,cx,y,cw,50,"Login screen background");
    int bw=cw-140;
    draw_input(cr,cx+12,y+14,bw,26,s->login_bg,s->focused==701,701);
    int hbg=in_btn(a->mx,a->my,cx+bw+16,y+14,110,26);
    draw_btn(cr,cx+bw+16,y+14,110,26,"Browse...",BTN_C,hbg,703);
    y+=64;

    if(s->dm==DM_SDDM){
        draw_group(cr,cx,y,cw,192,"SDDM Theme");
        fill_rect(cr,cx+12,y+18,200,162,6,col(10,12,22));
        stroke_rect(cr,cx+12,y+18,200,162,6,BORDER_C,1.0);
        cairo_save(cr);
        cairo_rectangle(cr,cx+12,y+18,200,162); cairo_clip(cr);
        for(int i=0;i<s->sddm_count&&i<7;i++){
            int iy=y+18+i*26;
            if(i==s->sddm_sel) fill_rect(cr,cx+12,iy,200,26,0,SEL_C);
            Col tc=i==s->sddm_sel?col(160,200,255):TEXT2;
            txt(cr,s->sddm_themes[i].name,cx+20,iy+18,tc,12,i==s->sddm_sel);
            btn_add(cx+12,iy,200,26,800+i);
        }
        cairo_restore(cr);
        y+=206;
    } else if(s->dm==DM_LIGHTDM){
        draw_group(cr,cx,y,cw,52,"Greeter LightDM");
        const char *gr[]={"lightdm-gtk-greeter","slick-greeter","unity-greeter",NULL};
        int gx=cx+12;
        for(int i=0;gr[i];i++){
            int sel=strcmp(s->ldm_greeter,gr[i])==0;
            int gw=(int)txtw(cr,gr[i],12,0)+22;
            int hg=in_btn(a->mx,a->my,gx,y+16,gw,26);
            fill_rect(cr,gx,y+16,gw,26,5,sel?SEL_C:BTN_C);
            stroke_rect(cr,gx,y+16,gw,26,5,BORDER_C,1.0);
            txt(cr,gr[i],gx+8,y+32,sel?col(160,200,255):TEXT2,12,sel);
            btn_add(gx,y+16,gw,26,900+i);
            gx+=gw+6;
        }
        y+=68;
    } else if(s->dm==DM_GDM){
        fill_rect(cr,cx,y,cw,42,5,col(20,26,26));
        txt(cr,"GDM: zmiana tla wymaga nadpisania CSS motywu lub narzedzia gdm-settings.",
            cx+12,y+27,TEXT2,11,0);
        y+=56;
    }

    if(s->login_status[0]){
        Col sc=s->login_ok?GREEN_C:RED_C;
        txt(cr,s->login_status,cx+10,CON_Y+CON_H-38,sc,12,0);
    }
    int hap=in_btn(a->mx,a->my,cx+cw-210,CON_Y+CON_H-54,198,38);
    draw_btn(cr,cx+cw-210,CON_Y+CON_H-54,198,38,"Zapisz konfiguracje",BTN_PRP_C,hap,704);
}

/* ── main draw ─────────────────────────────────────── */
static void draw_all(App *a){
    cairo_t *cr=a->cr; St *s=&a->s;
    btn_clear();

    /* bg */
    fill_rect(cr,0,0,WIN_W,WIN_H,0,BG);

    /* header */
    fill_rect(cr,0,0,WIN_W,HDR_H,0,HEADER);
    line(cr,0,HDR_H,WIN_W,HDR_H,BORDER_C,1);
    txt(cr,"  CookieOS Customizer",20,HDR_H/2+7,WHITE_C,18,1);
    const char *badge=s->is_root?"  ROOT  ":s->username;
    double bw=txtw(cr,badge,11,0)+20;
    fill_rect(cr,WIN_W-bw-16,HDR_H/2-12,bw,24,4,s->is_root?col(139,37,0):col(26,42,26));
    txt(cr,badge,WIN_W-bw-6,HDR_H/2+5,WHITE_C,11,0);

    /* sidebar */
    fill_rect(cr,0,HDR_H,NAV_W,CON_H,0,SIDEBAR);
    line(cr,NAV_W,HDR_H,NAV_W,WIN_H-SB_H,BORDER_C,1);
    const char *tabs[]={"GRUB","Desktop wallpaper","Login screen"};
    for(int i=0;i<3;i++){
        int ty=HDR_H+i*46;
        if(i==s->tab){
            fill_rect(cr,0,ty,NAV_W,46,0,SEL_C);
            src(cr,ACCENT_C); cairo_set_line_width(cr,3);
            cairo_move_to(cr,0,ty); cairo_line_to(cr,0,ty+46); cairo_stroke(cr);
            txt(cr,tabs[i],16,ty+29,col(160,196,255),13,1);
        } else {
            if(in_btn(a->mx,a->my,0,ty,NAV_W,46))
                fill_rect(cr,0,ty,NAV_W,46,0,col(28,34,60));
            txt(cr,tabs[i],16,ty+29,TEXT3,13,0);
        }
        btn_add(0,ty,NAV_W,46,10+i);
    }

    /* page content */
    switch(s->tab){
        case TAB_GRUB:  draw_grub(a);  break;
        case TAB_WALL:  draw_wall(a);  break;
        case TAB_LOGIN: draw_login(a); break;
    }

    /* status bar */
    fill_rect(cr,0,WIN_H-SB_H,WIN_W,SB_H,0,col(14,18,40));
    line(cr,0,WIN_H-SB_H,WIN_W,WIN_H-SB_H,BORDER_C,1);
    const char *den=s->de==DE_KDE?"KDE Plasma":s->de==DE_GNOME?"GNOME":
                    s->de==DE_XFCE?"XFCE":"Unknown";
    const char *dmn=s->dm==DM_SDDM?"SDDM":s->dm==DM_LIGHTDM?"LightDM":
                    s->dm==DM_GDM?"GDM":"Unknown";
    char sb[200]; snprintf(sb,sizeof(sb),"DE: %s   DM: %s   CookieOS Customizer v1.0.0",den,dmn);
    txt(cr,sb,14,WIN_H-8,TEXT3,11,0);
}

/* ── input handling ────────────────────────────────── */
static void handle_btn(App *a,int id){
    St *s=&a->s;
    char picked[512];

    if(id>=10&&id<=12){ s->tab=(Tab)(id-10); return; }

    /* GRUB */
    if(id==101){ if(s->grub_timeout>0)s->grub_timeout--; }
    if(id==102){ if(s->grub_timeout<120)s->grub_timeout++; }
    if(id==103){ chk_hidden=!chk_hidden; }
    if(id==108){ chk_osprober=!chk_osprober; }
    if(id>=104&&id<=107){ s->focused=id; }
    if(id>=300&&id<300+s->grub_theme_count){ s->grub_theme_sel=id-300; }

    if(id==201){ /* add theme dir */
        picked[0]=0;
        if(pick_path("Select GRUB theme directory",1,picked,sizeof(picked))&&picked[0])
            add_grub_theme_dir(s,picked);
        else if(!picked[0])
            strcpy(s->grub_status,"Type the theme directory path into the background field and press Enter");
    }
    if(id==202){ /* refresh theme list */
        int sel_save=s->grub_theme_sel;
        char sel_path[512]; strncpy(sel_path,s->grub_themes[sel_save].path,511);
        scan_grub_themes(s);
        /* try to restore selection */
        for(int i=0;i<s->grub_theme_count;i++)
            if(strcmp(s->grub_themes[i].path,sel_path)==0){s->grub_theme_sel=i;break;}
        snprintf(s->grub_status,sizeof(s->grub_status),
            "Refreshed - %d themes found",s->grub_theme_count-1);
        s->grub_ok=1;
    }
    if(id==203){ /* browse grub background */
        picked[0]=0;
        if(pick_path("Select GRUB background",0,picked,sizeof(picked))&&picked[0])
            strncpy(s->grub_background,picked,511);
        else s->focused=106;
    }
    if(id==204){ do_apply_grub(s); }

    /* Wallpaper */
    if(id==501){ s->focused=501; }
    if(id==502){ /* browse wallpaper dir */
        picked[0]=0;
        if(pick_path("Select a wallpaper directory",1,picked,sizeof(picked))&&picked[0]){
            scan_walls(s,picked);
            snprintf(s->wall_status,sizeof(s->wall_status),
                "Loaded %d wallpapers from: %s",s->wall_count,picked);
            s->wall_ok=1;
        } else s->focused=501;
    }
    if(id==503){ do_apply_wall(s); }
    if(id>=600&&id<600+s->wall_count){ s->wall_sel=id-600; }

    /* Login */
    if(id==701){ s->focused=701; }
    if(id==703){ /* browse login bg */
        picked[0]=0;
        if(pick_path("Select a login screen background",0,picked,sizeof(picked))&&picked[0])
            strncpy(s->login_bg,picked,511);
        else s->focused=701;
    }
    if(id==704){ do_apply_login(s); }
    if(id>=800&&id<800+s->sddm_count){ s->sddm_sel=id-800; }
    if(id>=900&&id<=902){
        const char *g[]={"lightdm-gtk-greeter","slick-greeter","unity-greeter"};
        strncpy(s->ldm_greeter,g[id-900],63);
    }
}

static char *input_target(St *s,size_t *sz){
    switch(s->focused){
        case 104: *sz=sizeof(s->grub_default);    return s->grub_default;
        case 105: *sz=sizeof(s->grub_resolution); return s->grub_resolution;
        case 106: *sz=sizeof(s->grub_background); return s->grub_background;
        case 107: *sz=sizeof(s->grub_cmdline);    return s->grub_cmdline;
        case 501: *sz=sizeof(s->wall_dir);        return s->wall_dir;
        case 701: *sz=sizeof(s->login_bg);        return s->login_bg;
    }
    *sz=0; return NULL;
}

static void handle_key(App *a,KeySym ks,const char *str){
    St *s=&a->s;
    if(!s->focused) return;
    size_t sz; char *t=input_target(s,&sz); if(!t)return;
    if(ks==XK_BackSpace){ size_t l=strlen(t); if(l)t[l-1]=0; }
    else if(ks==XK_Escape){ s->focused=0; }
    else if(ks==XK_Return){
        if(s->focused==501){ scan_walls(s,t); snprintf(s->wall_status,sizeof(s->wall_status),"Loaded %d wallpapers",s->wall_count); s->wall_ok=1; }
        s->focused=0;
    } else if(str&&str[0]>=32&&str[0]<127){
        size_t l=strlen(t); if(l<sz-1){t[l]=str[0];t[l+1]=0;}
    }
}

/* ── main ──────────────────────────────────────────── */
int main(int argc, char *argv[]){
    (void)argc;(void)argv;
    init_colors();

    App a; memset(&a,0,sizeof(a));
    a.s.wall_sel=-1;

    detect(&a.s);
    load_grub(&a.s);
    scan_grub_themes(&a.s);
    /* scan all known wallpaper dirs, use whichever has most images */
    {
        const char *wd[]={
            "/usr/share/wallpapers",
            "/usr/share/backgrounds",
            "/usr/share/pixmaps",
            NULL
        };
        int best=0;
        const char *bestdir=NULL;
        for(int i=0;wd[i];i++){
            if(access(wd[i],F_OK)!=0)continue;
            scan_walls(&a.s,wd[i]);
            if(a.s.wall_count>best){best=a.s.wall_count;bestdir=wd[i];}
        }
        if(bestdir&&best>0) scan_walls(&a.s,bestdir);
        else if(access("/usr/share/wallpapers",F_OK)==0)
            scan_walls(&a.s,"/usr/share/wallpapers");
    }
    scan_sddm(&a.s);
    strcpy(a.s.ldm_greeter,"lightdm-gtk-greeter");

    a.dpy=XOpenDisplay(NULL);
    if(!a.dpy){fprintf(stderr,"Cannot open display\n");return 1;}
    int scr=DefaultScreen(a.dpy);

    a.win=XCreateSimpleWindow(a.dpy,RootWindow(a.dpy,scr),
        (DisplayWidth(a.dpy,scr)-WIN_W)/2,
        (DisplayHeight(a.dpy,scr)-WIN_H)/2,
        WIN_W,WIN_H,0,
        BlackPixel(a.dpy,scr),BlackPixel(a.dpy,scr));

    XStoreName(a.dpy,a.win,"CookieOS Customizer");
    Atom wmdel=XInternAtom(a.dpy,"WM_DELETE_WINDOW",False);
    XSetWMProtocols(a.dpy,a.win,&wmdel,1);
    XSelectInput(a.dpy,a.win,
        ExposureMask|KeyPressMask|ButtonPressMask|PointerMotionMask|StructureNotifyMask);
    XMapWindow(a.dpy,a.win);

    a.surf=cairo_xlib_surface_create(a.dpy,a.win,
        DefaultVisual(a.dpy,scr),WIN_W,WIN_H);
    a.cr=cairo_create(a.surf);

    XEvent ev;
    a.need_redraw=1;
    int running=1;
    while(running){
        while(XPending(a.dpy)){
            XNextEvent(a.dpy,&ev);
            switch(ev.type){
                case Expose: a.need_redraw=1; break;
                case ConfigureNotify:
                    cairo_xlib_surface_set_size(a.surf,
                        ev.xconfigure.width,ev.xconfigure.height);
                    a.need_redraw=1; break;
                case MotionNotify:
                    a.mx=ev.xmotion.x; a.my=ev.xmotion.y;
                    a.need_redraw=1; break;
                case ButtonPress:
                    if(ev.xbutton.button==1){
                        int mx=ev.xbutton.x,my=ev.xbutton.y;
                        int hit=0;
                        for(int i=0;i<g_nb;i++){
                            if(in_btn(mx,my,g_btns[i].x,g_btns[i].y,
                                      g_btns[i].w,g_btns[i].h)){
                                handle_btn(&a,g_btns[i].id);
                                hit=1; a.need_redraw=1; break;
                            }
                        }
                        if(!hit) a.s.focused=0;
                    }
                    if(ev.xbutton.button==4&&a.scroll[a.s.tab]>0)
                        {a.scroll[a.s.tab]--;a.need_redraw=1;}
                    if(ev.xbutton.button==5)
                        {a.scroll[a.s.tab]++;a.need_redraw=1;}
                    break;
                case KeyPress:{
                    char buf[8]={0}; KeySym ks;
                    XLookupString(&ev.xkey,buf,sizeof(buf),&ks,NULL);
                    handle_key(&a,ks,buf);
                    a.need_redraw=1; break;}
                case ClientMessage:
                    if((Atom)ev.xclient.data.l[0]==wmdel)running=0; break;
            }
        }
        if(a.need_redraw){
            draw_all(&a);
            cairo_surface_flush(a.surf);
            XFlush(a.dpy);
            a.need_redraw=0;
        }
        usleep(16000);
    }
    cairo_destroy(a.cr);
    cairo_surface_destroy(a.surf);
    XDestroyWindow(a.dpy,a.win);
    XCloseDisplay(a.dpy);
    return 0;
}
