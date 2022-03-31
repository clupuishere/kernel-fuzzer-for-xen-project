/*
 * Copyright (C) 2020 Intel Corporation
 * SPDX-License-Identifier: MIT
 */

#include "private.h"
#include <arpa/inet.h>

#if defined(FZ_LINUX)
#include <errno.h>
#include <sys/syscall.h>
#else
#include <xen/xen.h>
#include <xenstore.h>
//#include <xenbus/xs.h>
//#include <xenbus/client.h>
#endif /* FZ_LINUX */


#define GET_TIME(start) (clock_gettime(CLOCK_REALTIME, &(start)))
#define TIME_DIFF(start, stop) ((stop.tv_sec - start.tv_sec) + ( stop.tv_nsec - start.tv_nsec )/ 1000000000.0);


static pthread_mutex_t mutex;
static pthread_cond_t cond;
static int ok;
static pthread_t setup_thread;
static pthread_t network_thread;

static char* vm_json_path = NULL;

static struct timespec vm_create_time;

/* void* wait_network(__attribute__((unused)) void *arg)
{

   static int port = 8888;
   char client_buf[BUFSIZ / 8];
   int server_fd, client_fd;
   struct sockaddr_in server_addr, client_addr;
   socklen_t client_len = sizeof(client_addr);

   server_fd = socket(PF_INET, SOCK_STREAM, 0);
   memset(&server_addr, 0, sizeof(server_addr));

   server_addr.sin_family = AF_INET;
   server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
   server_addr.sin_port = htons(port);

   int opt_val = 1;
   setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt_val, sizeof(opt_val));

   if (bind(server_fd, (struct sockaddr*) &server_addr, sizeof(server_addr)) < 0) {
       fprintf(stderr, "Could not bind socket\n");
   }

   if (listen(server_fd, 2) < 0 ) {
       fprintf(stderr, "Could not listen on socket\n");
   }
   fprintf(stderr, "Server is listening on %d\n", port);

   fd_set fds;
   FD_ZERO(&fds);
   FD_SET(server_fd, &fds);
   int max_fd = server_fd;

  while (1) {
     fd_set read_fds = fds;
     int rc = select(max_fd + 1, &read_fds, NULL, NULL, NULL);
     if (rc == -1) {
          fprintf(stderr, "select failed\n");
     }
				        
     for (int i = 0; i <= max_fd; ++i) {
          if (FD_ISSET(i, &read_fds)) {
            fprintf(stderr, "fd = %d\n", i);
            if (i == server_fd) {
               client_fd = accept(server_fd, (struct sockaddr*) &client_addr, &client_len);
               FD_SET(client_fd, &fds);
               if (client_fd > max_fd) {
                  max_fd = client_fd;
               }
            } else {
	      struct timespec end_time;
	      int bytes = recv(i, &end_time, sizeof(end_time), 0);
	      if (bytes <= 0) {
	          close(i);
	          FD_CLR(i, &fds);
	          continue;
	      }
	      double time_diff = TIME_DIFF(vm_create_time, end_time);
	      fprintf(stderr, "time network %lfms\n", time_diff * 1000);
            }
	}	
     }
   }

   pthread_exit(NULL);
} */


static int wait_ready(void)
{
   struct timespec start, stop;
   int rc;
   static const char* data_path = "/data/trigger-harness";
   int id = domid;

   if (handle == NULL) {
	handle = xs_open(0);
	 if (handle == NULL) {
	    fprintf(stderr, "Handle is null\n");
	    return -1;
         }
   }

   char* path = xs_get_domain_path(handle, id);
   if (!path) {
      fprintf(stderr, "xs_domain_path() failed\n");
      return -1;
   }

   char* tmp = realloc(path, strlen(path) + strlen(data_path) + 1);
   if (!tmp) {
      fprintf(stderr, "realloc() failed");
      return -1;
   }

   path = tmp;
   strcat(path, data_path);
   fprintf(stderr, "Watching %s\n", path);
    
   rc = xs_watch(handle, path, "ready");
   int fd = xs_fileno(handle);
   fd_set set;
   int num_strings;
   char **vec;
   // TODO: clock_get_time -> MACROS pentru masurat timp Expected: 30 ms
   while(1) {
   	 GET_TIME(start);
     	 FD_ZERO(&set);
	 FD_SET(fd, &set);
	 if (select(fd + 1, &set, NULL, NULL, NULL) > 0 && FD_ISSET(fd, &set)) {
	   GET_TIME(stop);
	   double  time_diff = TIME_DIFF(start, stop);
	   //fprintf(stderr, "select time %lfms\n", time_diff * 1000);
           vec = xs_read_watch(handle, &num_strings);
	   if (!vec) {
		fprintf(stderr, "xs_read_watch failed\n");
	   }
	   
	   unsigned int len;
	   char *buf = xs_read(handle, XBT_NULL, vec[XS_WATCH_PATH], &len);
	   if (buf) {
		if (!strcmp(buf, "ready")) {
		  char seconds_buf[100];
		  char nseconds_buf[100];
			
		  sprintf(seconds_buf, "/local/domain/%d/data/seconds", id);	  
		  sprintf(nseconds_buf, "/local/domain/%d/data/nseconds", id);	  

		  char* seconds = xs_read(handle, XBT_NULL, seconds_buf, &len);
		  char* nseconds = xs_read(handle, XBT_NULL, nseconds_buf, &len);
		  struct timespec vm_start;
		  sscanf(seconds, "%lu", &vm_start.tv_sec);
		  sscanf(nseconds, "%lu", &vm_start.tv_nsec);

		  fprintf(stderr, "%lu and %lu\n", vm_start.tv_sec, vm_start.tv_nsec);

		  time_diff = TIME_DIFF(vm_create_time, vm_start);
		  fprintf(stderr, "time xenstore %lfms\n", time_diff * 1000);
		  free(seconds);
		  free(nseconds);
		  break;
		}
	   }
	   free(buf);

	 }

   }

   free(path);
   return rc;

}

static void get_input(void)
{
    if ( !input_limit )
        return;

    if ( debug ) printf("Get %lu bytes of input from %s\n", input_limit, input_path);

    input_file = fopen(input_path, "r");
    if (!input_file){
        return;
    }

    input = malloc(input_limit);
    if ( !input ){
        fclose(input_file);
        input_file = NULL;
        return;
    }

    if ( !(input_size = fread(input, 1, input_limit, input_file)) )
    {
        free(input);
        input = NULL;
    }
    fclose(input_file);
    input_file = NULL;

    if ( debug ) printf("Got input size %lu\n", input_size);

}

extern int xc_cloning_reset(xc_interface *xch, uint32_t domid);

static bool inject_input(vmi_instance_t vmi)
{
    if ( !input || !input_size )
        return false;

    ACCESS_CONTEXT(ctx,
        .translate_mechanism = VMI_TM_PROCESS_DTB,
        .pt = target_pagetable,
        .addr = address
    );

   if ( debug ) fprintf(stdout, "Writing %lu bytes of input to 0x%lx\n", input_size, address);
   
     if ( vm_is_pv && (no_cloning == false))
     {
		if ( VMI_SUCCESS != pv_cow(vmi, fuzzdomid, address, NULL) ) {
			return false;
		}
    }

    if (debug) printf("Before return\n");
    return VMI_SUCCESS == vmi_write(vmi, &ctx, input_size, input, NULL);
}

static bool make_fuzz_ready()
{
    if ( !fuzzdomid )
        return false;

    if ( !setup_vmi(&vmi, NULL, fuzzdomid, json, true, true) )
    {
        fprintf(stderr, "Unable to start VMI on fuzz domain %u\n", fuzzdomid);
        return false;
    }

    // TODO: check if ptcov is enabled
    if ( ptcov && !setup_pt() )
    {
        fprintf(stderr, "Failed to enable Processor Tracing\n");
        return false;
    }

    setup_trace(vmi);

    if ( debug ) fprintf(stderr, "VM Fork is ready for fuzzing\n");

    return true;
}

static void* setup_vm(__attribute__((unused)) void *arg) {

    while(1) {
	pthread_mutex_lock(&mutex);
	while (ok == 0) {
		pthread_cond_wait(&cond, &mutex);
	}
	
	if (ok == 2) {
		break;
	}
	
	setup = true;
   	make_parent_ready(); 
  	setup = false;
	ok = 0;
	pthread_cond_signal(&cond);
	pthread_mutex_unlock(&mutex);

   }
   //setup = true;
   //make_parent_ready(); 
  // setup = false;
  pthread_exit(NULL);
}


static bool fuzz(void)
{

    fprintf(stderr, "Enter fuzz\n");
    fprintf(stderr, "Fuzzdommid = %d\n", fuzzdomid);
    if (no_cloning == false &&  !fuzzdomid )
        return false;

    fprintf(stderr, "Vmispv = %d\n", vm_is_pv);
    if ( vm_is_pv )
    {
	fprintf(stderr, "Resetting vm\n");
        if (no_cloning == false) {
        	if ( xc_cloning_reset(xc, fuzzdomid) )
            		return false;
	}
    }
    else
    if ( xc_memshr_fork_reset(xc, fuzzdomid) )
        return false;

	
    crash = 0;

    if ( afl )
    {
        afl_rewind();
        afl_instrument_location(start_rip);
        afl_wait();
    }

    get_input();

    if ( !start_trace(vmi, start_rip) )
        return false;
    if ( !inject_input(vmi) )
    {
        fprintf(stderr, "Injecting input failed\n");
        return false;
    }

    if ( debug ) fprintf(stderr, "Starting fuzz loop\n");
    loop(vmi);
    if ( debug ) fprintf(stderr, "Stopping fuzz loop.\n");

    if ( ptcov )
        decode_pt();

    vmi_pagecache_flush(vmi);
    if ( vm_is_pv )
        vmi_v2pcache_flush(vmi, ~0ull);
    else
    vmi_v2pcache_flush(vmi, target_pagetable);
    vmi_pidcache_flush(vmi);
    vmi_rvacache_flush(vmi);
    vmi_symcache_flush(vmi);

    bool ret = false;
    fprintf(stderr, "Crash = %s\n", crash ? "yes" : "no");
    if ( afl )
    {
        afl_report(crash);
        ret = true;
    }
    else if ( loopmode )
    {
        if ( crash )
        {
            FILE *f = fopen("crash.out","w+");
            fwrite(input, input_size, 1, f);
            fclose(f);
            ret = false;
	    close_trace(vmi);
	    vmi_destroy(vmi);
	    xc_domain_destroy(xc, fuzzdomid);
        } else
            ret = true;
    } else
        fprintf(stderr, "Result: %s\n", crash ? "crash" : "no crash");

    free(input);
    input = NULL;

    fprintf(stderr, "After copy %d\n", no_cloning);
    if (no_cloning == true) {
	xc_dominfo_t dominfo;
	int rc;
	rc = xc_domain_getinfo(xc, domid, 1, &dominfo);
	// if no, something wrong happened
	if (rc < 0) {
	   fprintf(stderr, "xc_domain_getinfo\n");
	   return false;
	} else {
	   close_trace(vmi);
	   vmi_destroy(vmi);

	   char buffer[1000] = {};
	   struct timespec start, stop;
	   double time_elapsed;

	   sprintf(buffer, "xl destroy %d\n", domid);
	   while (xc_domain_getinfo(xc, domid, 1, &dominfo) > 0) {
	   	system(buffer);
	   }
	   domid = domid + 1;

	   memset(buffer, 0, 1000);
	   sprintf(buffer, "xl create -q -e %s\n", vm_json_path);
	   fprintf(stderr, "buffer is %s\n", buffer);
	   GET_TIME(vm_create_time);
	   
	   system(buffer);

           //fprintf(stderr, "time-xl-create:%lf\nms", time_elapsed * 1000);


	   //memset(buffer, 0, 1000);
	   //sprintf(buffer, "sleep 4\n");
	   //system(buffer);

	   GET_TIME(start);
	   wait_ready();
	   GET_TIME(stop);
	  

	   time_elapsed = TIME_DIFF(start, stop);

           fprintf(stderr, "time-while:%lf\n", time_elapsed * 1000);


	   //memset(buffer, 0, 1000);
	   //sprintf(buffer, "sleep 4\n");
	   //system(buffer);



	   //setup = true;
	   //domid = domid + 1;
           //make_parent_ready(); 
           //setup = false;
	   //fprintf(stderr, "after setup\n");
	   pthread_mutex_lock(&mutex);	
	   while (ok == 1) {
		pthread_cond_wait(&cond, &mutex);
	   }
	   ok = 1;
	   pthread_cond_signal(&cond);
	   pthread_mutex_unlock(&mutex);


           //pthread_attr_t attr;
	   //pthread_t thread;

	   //pthread_attr_init(&attr);
	   //pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
           //pthread_create(&thread, &attr, &setup_vm, NULL);
	   //pthread_attr_destroy(&attr);

                     
           memset(buffer, 0, 1000);
	   sprintf(buffer, "xenstore-write /local/domain/%d/data/trigger-harness done\n", domid);
	   system(buffer);

	  // usleep(20000);

	   //memset(buffer, 0, 1000);
	   //sprintf(buffer, "sleep 2\n");
	   //system(buffer);   
	  
//	   domid = fuzzdomid + 1;

           // setup = true;
    	   // make_parent_ready(); 
   	   // setup = false;
	   pthread_mutex_lock(&mutex);	
	   while (ok == 1) {
		pthread_cond_wait(&cond, &mutex);
	   }
	   pthread_cond_signal(&cond);
	   pthread_mutex_unlock(&mutex);


	   fuzzdomid = domid;
	   sinkdomid = domid;
	   xc_domain_fuzzing_enable(xc, domid); 
	   make_parent_ready();
	   make_sink_ready();
	   make_fuzz_ready();
	}
	//domid = 0;
	//make_parent_ready();
	//xc_domain_fuzzing_enable(xc, domid);
	//make_sink_ready();
	//make_fuzz_ready();
    }


    return ret;
}

static bool validate_flags()
{
    if ( !domain && !domid )
    {
        fprintf(stderr, "Must specify --domain OR --domid of parent VM\n");
        return false;
    }
    if ( !setup && ((!address == !extended_mark) || (!input_limit == !extended_mark)) )
    {
        fprintf(stderr, "Must exclusively specify either (--address AND --input-limit) of target buffer OR --extended-mark\n");
        return false;
    }
    if ( !setup && !input_path )
    {
        fprintf(stderr, "Must specify --input path of taget buffer\n");
        return false;
    }
    if ( !setup && !json && !sink_list )
    {
        fprintf(stderr, "Must specify either kernel sym --json OR --sink-vaddr/--sink-paddr per sink\n");
        return false;
    }
    return true;
}

static void usage(void)
{
    printf("Inputs required for SETUP step:\n");
    printf("\t  --setup\n");
    printf("\t  --domain <domain name> OR --domid <domain id>\n");
    printf("\tOptional inputs:\n");
    printf("\t  --harness cpuid|breakpoint (default is cpuid)\n");
    printf("\t  --magic-mark <magic number signaling start harness> (default is 0x13371337)\n");
    printf("\t  --extended-mark (Use start harness to obtain target address & size)\n");
    printf("\t  --start-byte <byte> (used to replace the starting breakpoint harness)\n");

    printf("\n\n");
    printf("Inputs required for FUZZING step:\n");
    printf("\t  --input <path to input file> or @@ with AFL\n");
    printf("\t  --input-limit <limit input size> (XOR --extended-mark)\n");
    printf("\t  --address <kernel virtual address to inject input to> (XOR --extended-mark)\n");
    printf("\t  --domain <domain name> OR --domid <domain id>\n");
    printf("\t  --json <path to kernel debug json> (needed only if default sink list is used or --sink is used)\n");
    printf("\tOptional inputs:\n");
    printf("\t  --extended-mark (Use start harness to obtain target address & size)\n");
    printf("\t  --limit <limit FUZZING execution to # of CF instructions>\n");
    printf("\t  --harness cpuid|breakpoint (default is cpuid)\n");
    printf("\t  --loopmode (Run in a loop without coverage trace, for example using /dev/urandom as input)\n");
    printf("\t  --refork <create new fork after # of executions>\n");
    printf("\t  --keep (keep VM fork after kfx exits)\n");
    printf("\t  --nocov (disable coverage tracing)\n");
    printf("\t  --ptcov (use IPT coverage tracing)\n");
    printf("\t  --detect-doublefetch <kernel virtual address on page to detect doublefetch>\n");
    printf("\t  --sink <function_name>\n");
    printf("\t  --sink-vaddr <virtual address>\n");
    printf("\t  --sink-paddr <physical address>\n");
    printf("\t  --record-codecov <path to save file>\n");
    printf("\t  --no-cloning (only for Unikraft\n");

    printf("\n\n");
    printf("Optional global inputs:\n");
    printf("\t--debug\n");
    printf("\t--logfile <path to logfile>\n");
}

int main(int argc, char** argv)
{
    char *logfile = NULL;
    int c, out = 0, long_index = 0;
    printf("test rn\n");
    const struct option long_opts[] =
    {
        {"help", no_argument, NULL, 'h'},
        {"domain", required_argument, NULL, 'd'},
        {"domid", required_argument, NULL, 'i'},
        {"json", required_argument, NULL, 'j'},
        {"input", required_argument, NULL, 'f'},
        {"input-limit", required_argument, NULL, 'L'},
        {"address", required_argument, NULL, 'a'},
        {"limit", required_argument, NULL, 'l'},
        {"setup", no_argument, NULL, 's'},
        {"debug", no_argument, NULL, 'v'},
        {"logfile", required_argument, NULL, 'F'},
        {"harness", required_argument, NULL, 'H'},
        {"start-byte", required_argument, NULL, 'S'},
        {"refork", required_argument, NULL, 'r'},
        {"loopmode", no_argument, NULL, 'O'},
        {"keep", no_argument, NULL, 'K'},
        {"nocov", no_argument, NULL, 'N'},
        {"ptcov", no_argument, NULL, 't'},
        {"detect-doublefetch", required_argument, NULL, 'D'},
        {"magic-mark", required_argument, NULL, 'm'},
        {"extended-mark", no_argument, NULL, 'c'},
        {"sink", required_argument, NULL, 'n'},
        {"sink-vaddr", required_argument, NULL, 'V'},
        {"sink-paddr", required_argument, NULL, 'P'},
        {"record-codecov", required_argument, NULL, 'R'},
        {"record-memaccess", required_argument, NULL, 'M'},
        {"os", required_argument, NULL, 'o'},
	{"no-cloning", required_argument, NULL, 'z'},
        {NULL, 0, NULL, 0}
    };
    const char* opts = "d:i:j:f:a:l:F:H:S:m:n:V:P:R:M:z:svchtOKNDo:";
    limit = ~0;
    unsigned long refork = 0;
    bool keep = false;
    bool default_magic_mark = true;


    debug = true;

    address = 0;
    magic_mark = 0;
    harness_cpuid = true;
    input_path = NULL;
    input_size = 0;
    input_limit = 0;

    while ((c = getopt_long (argc, argv, opts, long_opts, &long_index)) != -1)
    {
    	printf("char is %c\n", c);
        switch(c)
        {
        case 'd':
            domain = optarg;
            break;
        case 'i':
            domid = strtoul(optarg, NULL, 0);
            break;
        case 'j':
            json = optarg;
            break;
        case 'f':
            input_path = optarg;
            break;
        case 'a':
            address = strtoull(optarg, NULL, 0);
            break;
        case 'l':
            limit = strtoull(optarg, NULL, 0);
            break;
        case 'L':
            input_limit = strtoull(optarg, NULL, 0);
            break;
        case 's':
            setup = true;
            break;
        case 'v':
            debug = true;
            break;
        case 'F':
            logfile = optarg;
            break;
        case 'H':
            if ( !strcmp(optarg, "breakpoint") )
                harness_cpuid = false;
            break;
        case 'S':
            start_byte = strtoull(optarg, NULL, 0);
            break;
        case 'r':
            refork = strtoull(optarg, NULL, 0);
            break;
        case 'O':
            loopmode = true;
            nocov = true;
            break;
        case 'K':
            keep = true;
            break;
        case 'N':
            nocov = true;
            break;
        case 't':
            ptcov = true;
            break;
	case 'z': // Unikraft no cloning
	    no_cloning = true;
	    //no_cloning = false;
	    vm_json_path = strdup(optarg);
	    break;
        case 'D':
            doublefetch = g_slist_prepend(doublefetch, GSIZE_TO_POINTER(strtoull(optarg, NULL, 0)));
            break;
        case 'm':
            default_magic_mark = false;
            magic_mark = strtoul(optarg, NULL, 0);
            break;
        case 'c':
            extended_mark = true;
            break;
        case 'n':
        {
            struct sink *s = g_malloc0(sizeof(struct sink));
            s->function = optarg;
            sink_list = g_slist_prepend(sink_list, s);
            break;
        }
        case 'V':
        {
            struct sink *s = g_malloc0(sizeof(struct sink));
            s->vaddr = strtoull(optarg, NULL, 0);
            sink_list = g_slist_prepend(sink_list, s);
            break;
        }
        case 'P':
        {
            struct sink *s = g_malloc0(sizeof(struct sink));
            s->paddr = strtoull(optarg, NULL, 0);
            sink_list = g_slist_prepend(sink_list, s);
            break;
        }
        case 'R':
            record_codecov = optarg;
            break;
        case 'M':
            record_memaccess = optarg;
            break;
        case 'o':
            libvmi_default_os = optarg;
            break;
        case 'h': /* fall-through */
        default:
            usage();
            return -1;
        };
    }

    if ( !validate_flags() )
    {
        usage();
        return -1;
    }

    if ( !harness_cpuid )
    {
        if ( !start_byte )
        {
            printf("For breakpoint harness --start-byte with a value must be provided (NOP is always a good option, 0x90)\n");
            return -1;
        }

        if ( default_magic_mark )
            magic_mark = 0;
    } else if ( default_magic_mark && setup )
        magic_mark = 0x13371337;


    if ( logfile )
    {
        out = open(logfile, O_RDWR|O_CREAT|O_APPEND, 0600);
        if (-1 == dup2(out, fileno(stdout))) { close(out); return -1; }
        if (-1 == dup2(out, fileno(stderr))) { close(out); return -1; }
    }

    if ( debug ) printf ("############ START ################\n");

    setup_handlers();

    bool parent_ready = make_parent_ready();
    printf("Parent is ready from main %d\n", parent_ready);
    printf("Setup variabile is %d\n", setup);
	
    if ( setup )
    {
        if ( logfile ) close(out);
        return parent_ready ? 0 : -1;
    }

    printf("After setup main\n");
    if ( !parent_ready )
        goto done;

    printf("After parent ready main\n");
    if ( !(xc = xc_interface_open(0, 0, 0)) )
    {
        fprintf(stderr, "Failed to grab xc interface\n");
        goto done;
    }
    /*
     * To reduce the churn of placing the sink breakpoints into the VM fork's memory
     * for each fuzzing iteration (which requires full-page copies for each breakpoint)
     * we create a fork that will only be used to house the breakpointed sinks,
     * ie. sinkdomid. We don't want to place the breakpoints in the parent VM
     * since that would prohibit other kfx instances from running on the domain
     * with potentially other sinkpoints.
     *
     * Fuzzing is performed from a further fork made from sinkdomid, in fuzzdomid.
     */

    xc_domain_fuzzing_enable(xc, domid);
    printf("vm is pv main: %d\n", vm_is_pv);
    if ( vm_is_pv )
    {
        int rc;
        printf("main domid is %d\n", domid);
	if (no_cloning == false) {
        	rc = xc_domain_fuzzing_enable(xc, domid);
        	if (rc) {
            		perror("Error calling xc_domain_fuzzing_enable()");
            		goto done;
        	}


		rc = xc_cloning_clone_single(xc, domid, &sinkdomid);
		if (rc) {
		    perror("Error calling xc_cloning_clone_single()");
		    goto done;
		}

		rc = xc_domain_fuzzing_enable(xc, sinkdomid);
		if (rc) {
		    perror("Error calling xc_domain_fuzzing_enable()");
		    goto done;
		}

		if ( !make_sink_ready() )
		{
		    fprintf(stderr, "Seting up sinks on VM fork domid %u failed\n", sinkdomid);
		    goto done;
		}

		rc = xc_cloning_clone_single(xc, sinkdomid, &fuzzdomid);
		if (rc) {
		    perror("Error calling xc_cloning_clone_single()");
		    goto done;
		}

		rc = xc_domain_fuzzing_enable(xc, fuzzdomid);
		if (rc) {
		    perror("Error calling xc_domain_fuzzing_enable()");
		    goto done;
		}	
	} else {
		sinkdomid = domid;
		fuzzdomid = domid;
	}

    }
    else
    {
    if ( !fork_vm(domid, &sinkdomid) )
    {
        fprintf(stderr, "Domain fork failed, sink domain not up\n");
        goto done;
    }

    if ( !fork_vm(sinkdomid, &fuzzdomid) )
    {
        fprintf(stderr, "Domain fork failed, fuzz domain not up\n");
        goto done;
    }
    }

    afl_setup();
    printf("after afl setup\n");

    if ( !afl )
    {
        input_file = fopen(input_path,"r"); // Sanity check
        if ( !input_file )
        {
            fprintf(stderr, "Failed to open input file %s\n", input_path);
            goto done;
        }
        fclose(input_file); // Closing for now, will reopen when needed
        

	if (no_cloning == false) {
		printf("Fork VMs created: %u -> %u -> %u\n", domid, sinkdomid, fuzzdomid);
	}
     }

    input_file = NULL;


    printf("no_cloning = %d\n", no_cloning);
    if ( !make_sink_ready() )
    {
        fprintf(stderr, "Seting up sinks on VM fork domid %u failed\n", sinkdomid);
        goto done;
    }

    if ( !nocov && !ptcov && cs_open(CS_ARCH_X86, pm == VMI_PM_IA32E ? CS_MODE_64 : CS_MODE_32, &cs_handle) )
    {
        fprintf(stderr, "Capstone init failed\n");
        goto done;
    }

    if (!make_fuzz_ready() )
    {
        fprintf(stderr, "Seting up fuzzing on VM fork domid %u failed\n", fuzzdomid);
        goto done;
    }

    if ( debug ) printf("Starting fuzzer on %u\n", fuzzdomid);

    if ( loopmode ) printf("Running in loopmode\n");
    else if ( afl )  printf("Running in AFL mode\n");
    else printf("Running in standalone mode\n");

    unsigned long iter = 0, t = time(0), cycle = 0;


    if (no_cloning) {
    	 int rc;
	 rc = pthread_mutex_init(&mutex, NULL);
	 if (rc == -1) {
		 fprintf(stderr, "pthread_mutex_init failed\n");
		 return -1;
	 }
	 rc = pthread_cond_init(&cond, NULL);
         if (rc == -1) {
               fprintf(stderr, "pthread_cond_init failed\n");
               return -1;
         }

    	pthread_create(&setup_thread, NULL, &setup_vm, NULL);
	// pthread_create(&network_thread, NULL, &wait_network, NULL);
    }
    
    while ( fuzz() )
    {
        iter++;

        if ( loopmode )
        {
            unsigned long now = time(0);
            if (t != now)
            {
                printf("Completed %lu iterations\n", iter - cycle);
                t = now;
                cycle = iter;
            }
        }

	 
        printf("Completed %lu iterations and refork is %ld \n", iter - cycle, refork);
        if ( iter == refork )
        {
	    printf("Iter == refork\n");
            close_trace(vmi);
            vmi_destroy(vmi);
	    if (no_cloning == false) {
		xc_domain_destroy(xc, fuzzdomid);
	    }

            iter = 0;
            fuzzdomid = 0;

            if ( fork_vm(sinkdomid, &fuzzdomid) )
                make_fuzz_ready();
        }
    }

    close_trace(vmi);
    vmi_destroy(vmi);
    fprintf(stderr, "Called vmi_destroy\n");

done:
    printf("reached done\n");
    if ( ptcov )
        close_pt();
    if ( fuzzdomid && !keep  && no_cloning == false)
        xc_domain_destroy(xc, fuzzdomid);
    if ( sinkdomid && !keep && no_cloning == false)
        xc_domain_destroy(xc, sinkdomid);

    if ( sink_list )
    {
        if ( !builtin_list )
            g_slist_free_full(sink_list, g_free);
        else
            g_slist_free(sink_list);
    }

    xc_interface_close(xc);
    cs_close(&cs_handle);
    if ( input_file )
        fclose(input_file);

    if ( debug ) printf(" ############ DONE ##############\n");
    if ( logfile )
        close(out);

    
    xs_close(handle);

    if (no_cloning) {
      	pthread_mutex_lock(&mutex);
    	if (ok == 1) {
        	pthread_cond_wait(&cond, &mutex);
    	}
    	ok = 2;
    	pthread_cond_signal(&cond);
    	pthread_mutex_unlock(&mutex);

    	pthread_join(setup_thread, NULL);
	//pthread_join(network_thread, NULL);
    	pthread_mutex_destroy(&mutex);
    	pthread_cond_destroy(&cond);
    }

       free(vm_json_path);
       return 0;
}
