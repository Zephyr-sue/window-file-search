#include<stdio.h>
#include<dirent.h>
#include<string.h>
#include<sys/stat.h>
// changes
void crawl(char *path){

    DIR *dir = opendir(path);

    if(dir == NULL){
        return;
    }

    struct dirent *entry;

    while((entry = readdir(dir)) != NULL){

        if(strcmp(entry->d_name,".") == 0 || strcmp(entry->d_name,"..") == 0){
            continue;
        }

        char full_path[1024];
        sprintf(full_path,"%s/%s",path,entry->d_name);
        
        struct stat st;

        if(stat(full_path,&st) == -1){
            continue;
        }

        if(S_ISREG(st.st_mode)){
            printf("%s\n",full_path);
        }

        else if(S_ISDIR(st.st_mode)){
            crawl(full_path);
        }
    }

    closedir(dir);
}

int main(int argc,char *argv[]){

    if(argc < 2){
        printf("Usage : ./crawler <directory>\n");
        return 1;
    }

    crawl(argv[1]);

    return 0;
}