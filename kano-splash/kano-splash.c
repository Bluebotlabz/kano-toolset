//-------------------------------------------------------------------------
//
// The MIT License (MIT)
//
// Copyright (c) 2013 Andrew Duncan
// Copyright (c) 2015 Kano Computing Ltd.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
// IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
// CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
// TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
// SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
//-------------------------------------------------------------------------

#define _GNU_SOURCE

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

#include "backgroundLayer.h"
#include "imageLayer.h"
#include "loadpng.h"
#include "get-start-time.h"
#include "kano-log.h"

#include "bcm_host.h"

//-------------------------------------------------------------------------

#define NDEBUG

//-------------------------------------------------------------------------


//-------------------------------------------------------------------------

void usage(void)
{
  fprintf(stderr, "Usage: kano-splash-start [-b <RGBA>] [ -t <timeout> ] <file.png>\n");
  fprintf(stderr, "    -b - set background colour 16 bit RGBA, in 0x1234 notation\n");
  fprintf(stderr, "    -t - timeout in seconds\n");
  fprintf(stderr, "         e.g. background 0x000F is opaque black\n");
  fprintf(stderr, "    Parameters in shebang are passed with the ampersand delimiter, e.g.\n");
  fprintf(stderr, "    #!/usr/bin/kano-splash /path/my/image.png&t=5&b=0x00ff /bin/bash\n");

  exit(EXIT_FAILURE);
}

int display_image( char *file, uint16_t timeout, uint16_t background)
{
    int error = 0;
    // don't allow us to be killed, because this causes a videocore memory leak
    signal(SIGKILL,SIG_IGN); 

    sigset_t waitfor;
    sigaddset(&waitfor,SIGALRM);
    sigprocmask(SIG_BLOCK,&waitfor,NULL); // switch off ALRM in case we are sent it before we want
    //---------------------------------------------------------------------

    bcm_host_init();

    //---------------------------------------------------------------------

    DISPMANX_DISPLAY_HANDLE_T display = vc_dispmanx_display_open(0);
    if(!display){
        error=1;
        kano_log_error("kano-splash: failed to open display\n");
        goto end;
    }

    //---------------------------------------------------------------------

    DISPMANX_MODEINFO_T info;
    int result = vc_dispmanx_display_get_info(display, &info);
    if(result != DISPMANX_SUCCESS){
	error = 1;
	kano_log_error("kano-splash: failed to get display info\n");
	goto close_display;
    }

    //---------------------------------------------------------------------

    BACKGROUND_LAYER_T backgroundLayer;
    bool okay = initBackgroundLayer(&backgroundLayer, background, 0);
    if(!okay){
         kano_log_error("kano-splash: failed to init background layer\n");
	 error=1;
	 goto close_display;
    }

    IMAGE_LAYER_T imageLayer;
    if (loadPng(&(imageLayer.image), file) == false)
    {
        kano_log_error("kano-splash: unable to load %s\n",file);
	error=1;
	goto close_background;

    }
    okay = createResourceImageLayer(&imageLayer, 1);
    if(!okay){
	 
        kano_log_error("kano-splash: unable to create resource \n");
        error=1;
	goto close_background;
    }
	 
	 

    //---------------------------------------------------------------------

    int32_t outWidth = imageLayer.image.width;
    int32_t outHeight = imageLayer.image.height;

    if ((imageLayer.image.width > info.width) &&
        (imageLayer.image.height > info.height))
    {
    }
    else
    {
        double imageAspect = (double)imageLayer.image.width
                                     / imageLayer.image.height;
        double screenAspect = (double)info.width / info.height;

        if (imageAspect > screenAspect)
        {
            outWidth = info.width;
            outHeight = (imageLayer.image.height * info.width)
                         / imageLayer.image.width; 
        }
        else
        {
            outWidth = (imageLayer.image.width * info.height)
                        / imageLayer.image.height; 
            outHeight = info.height;
        }
    }

    vc_dispmanx_rect_set(&(imageLayer.srcRect),
                         0 << 16,
                         0 << 16,
                         imageLayer.image.width << 16,
                         imageLayer.image.height << 16);

    vc_dispmanx_rect_set(&(imageLayer.dstRect),
                         (info.width - outWidth) / 2,
                         (info.height - outHeight) / 2,
                         outWidth,
                         outHeight);

    //---------------------------------------------------------------------

    DISPMANX_UPDATE_HANDLE_T update = vc_dispmanx_update_start(0);
    if(!update) {
         kano_log_error("kano-splash: unable to start update\n");
	 error = 1;
	 goto close_imagelayer;
    }
	 

    bool res=addElementBackgroundLayer(&backgroundLayer, display, update);
    if(!res) {
         kano_log_error("kano-splash: unable to add background element\n");
	 error=1;
	 goto close_imagelayer;
    }

    res=addElementImageLayerCentered(&imageLayer, &info, display, update);
    if(!res) {
         kano_log_error("kano-splash: unable to add foreground element\n");
	 error=1;
	 goto close_imagelayer;
    }

    result = vc_dispmanx_update_submit_sync(update);
    if(result != DISPMANX_SUCCESS) {
         kano_log_error("kano-splash: unable to submit update\n");
	 error = 1;
	 goto close_imagelayer;
    }
    //---------------------------------------------------------------------
    // wait to be signalled with SIGARLM or timeout
    struct timespec ts;
    ts.tv_sec=timeout;
    ts.tv_nsec=0;
      
    sigtimedwait(&waitfor,NULL, &ts);
   
    //---------------------------------------------------------------------


    //---------------------------------------------------------------------

close_imagelayer:

    destroyImageLayer(&imageLayer);

close_background:
    
    destroyBackgroundLayer(&backgroundLayer);

    //---------------------------------------------------------------------
close_display:
    
    result = vc_dispmanx_display_close(display);

    //---------------------------------------------------------------------
end:
    return error;
}

//-------------------------------------------------------------------------

int main(int argc, char *argv[])
{
    uint16_t background = 0x000F;
    uint16_t timeout = 15;
    char *real_interp;
    char *file;
    char *binary;
    int is_interp;
    int status;
    int error=0;
    pid_t command_child_pid=0;
    pid_t image_child_pid=0;
    int launch_command=false;

    binary=basename(argv[0]);
    is_interp=strcmp(binary,"kano-splash")==0;
      
    if(!is_interp){

        /* we are called from the command line */
        int opt=0;

        while ((opt = getopt(argc, argv, "b:t:h?")) != -1)
        {
            switch(opt)
            {
            case 'b':
        
                background = strtol(optarg, NULL, 16);
                break;
        
            case 't':
        
                timeout = strtol(optarg, NULL, 10);
                break;
        
            default:
        
                usage();
                break;
            }
        }
        file=argv[optind];

        //---------------------------------------------------------------------
   
        if (optind >= argc)
        {
            usage();
        }
    }
    else{

        /* we are called from a shebang */

        // Parse command arguments
        // As we are acting as an 'interpreter' arguments are as follows:
        // argv[0] path to this binary
        // argv[1] arguments on the #! line, if any (otherwise skipped..)
        // argv[2] the script we are called on
        // argv[3..] command line arguments
    
        // we expect argv[1] to contain the filename followed by the next interpreter we want to invoke and
        // its arguments. so we extract these and then do the following mapping:
    
        // splash file = arg[1][0]
        // argv[0] = argv[1][1..]
        // 
        // This leaves the command line 
        
    
        background=0; // full transparency        
    
        if(argc<2) {
          fprintf(stderr,"Insufficient arguments\n");
          exit(1);
        }

        /* find the image filename in the shebang */
        file=strsep(&argv[1]," ");
        if(!file) {
          fprintf(stderr,"filename is NULL \n");      
	  exit(1);
        }
        else {

          /* try to find additional arguments in a custom shebang syntax */
          /* example: #!/usr/bin/kano-splash /my/path/file.png&t=30&b=255 /bin/bash */
          char *token=strtok(file, "&");

          // if we have arguments, isolate the filename, then start parsing.
          file=(token ? token : file);

          while (token) {

              if (!strncmp(token, "t=", 2)) {
                  timeout = strtol(&token[2], NULL, 10);
              }
              else if (!strncmp(token, "b=", 2)) {
                  background = strtol(&token[2], NULL, 16);
              }

              token=strtok(NULL, "&");
          }
        }
    
        real_interp=strsep(&argv[1]," ");
          
        if(!real_interp) {
          fprintf(stderr,"real interpreter is not specified!\n");
          exit(1);
        }
    
        if(argv[1]==0){
          argv++;
          argc--;
        }

        // save back to argv for exec
        argv[0]=real_interp;

        launch_command = true;
    
    }

    image_child_pid = fork();
    if(image_child_pid==0){
      error = display_image(file, timeout, background);
      exit(error);
    }

    if(launch_command){
	// save pid info so child can halt the splash
	char pid_str[32];
	char start_time_str[32];
	snprintf(pid_str,32,"%u",image_child_pid);
	snprintf(start_time_str,32,"%llu",get_process_start_time(image_child_pid));
	// set environment variables for stopping the splash
	setenv("SPLASH_PID",pid_str,1);
	setenv("SPLASH_START_TIME",start_time_str,1);

	// launch command
        command_child_pid=fork();
        if(command_child_pid==0){
          execvp(real_interp,argv);	
        }
    }      
    error = waitpid(image_child_pid, &status, 0);

    if(command_child_pid){
      int r = waitpid(command_child_pid, &status, 0);
      if(r){
        kano_log_error("kano-splash: error from child\n");
      }
    }
    return error;
}

