#include "main-conf.h"
#include "string_s.h"
#include "logger.h"
#include "util-ipaddr.h"
#include "zonefile-parse.h"
#include "zonefile-load.h"
#include "success-failure.h"
#include "pixie.h"
#include "pixie-nic.h"
#include "pixie-timer.h"
#include "pixie-threads.h"
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <sys/stat.h>

/****************************************************************************
 * This function parses the zone-file. Since parsing can take a long time,
 * such as when reading the .com file, we print status indicating how long
 * things are taking.
 ****************************************************************************/
enum SuccessFailure
zonefile_benchmark(
        struct DomainPointer domain,
        struct DomainPointer origin,
	    unsigned type,
        unsigned ttl,
        unsigned rdlength,
        const unsigned char *rdata,
        uint64_t filesize,
	    void *userdata,
        const char *filename,
        unsigned line_number)
{
    return Success;
}

/****************************************************************************
 * Look for suffixes to strings, especially looking for file types like
 * ".conf" or ".zone" or ".pcap".
 * @return 1 if the string has that suffix, or 0 otherwise.
 ****************************************************************************/
static int
ends_with(const char *string, const char *suffix)
{
    size_t string_length = strlen(string);
    size_t suffix_length = strlen(suffix);

    if (suffix_length > string_length)
        return 0;

    return memcmp(string+string_length-suffix_length, suffix, suffix_length) == 0;
}

/****************************************************************************
 ****************************************************************************/
static char *
combine_filename(const char *dirname, const char *filename)
{
    size_t dirname_len = strlen(dirname);
    size_t filename_len = strlen(filename);
    char *xfilename = malloc(dirname_len + filename_len + 2);

    memcpy(xfilename, dirname, dirname_len);

    while (dirname_len && (xfilename[dirname_len-1] == '/' || xfilename[dirname_len-1] == '\\'))
        dirname_len--;

    xfilename[dirname_len++] = '/';
    memcpy(xfilename + dirname_len, filename, filename_len);
    xfilename[dirname_len + filename_len] = '\0';

    return xfilename;
}



/****************************************************************************
 ****************************************************************************/
static void
conf_zonefile_addname(struct Core *core, 
        const char *dirname, size_t dirname_length, 
        const char *filename)
{
    size_t filename_length;
    size_t *filenames_length = &core->zonefiles.length;
    size_t *filenames_max = &core->zonefiles.max;
    char *filenames = core->zonefiles.names;
    size_t original_offset = core->zonefiles.length;

    /* expand filename storage if needed */
    filename_length = strlen(filename);
    while (*filenames_length + dirname_length + filename_length + 2 > *filenames_max) {
        *filenames_max *= 2 + 1;
        filenames = realloc(filenames, *filenames_max + 2);
        core->zonefiles.names = filenames;
    }

    /* Append filenames -- even if we aren't going to use it.
        * We may decide below we on't want it and revert back */
    memcpy(filenames + *filenames_length,
            dirname,
            dirname_length);
    *filenames_length += dirname_length;
        
    filenames[*filenames_length] = '/';
    (*filenames_length)++;

    memcpy(filenames + *filenames_length,
            filename,
            filename_length);
    *filenames_length += filename_length;

    filenames[*filenames_length] = '\0';
    (*filenames_length)++;
    filenames[*filenames_length] = '\0'; /* double nul terminate list */

    core->zonefiles.total_files++;

    LOG(1, "added: %s\n", core->zonefiles.names + original_offset);
}

/****************************************************************************
 * Recursively descened a file directory tree and create a list of 
 * all filenames ending in ".zone".
 ****************************************************************************/
void
directory_to_zonefile_list(struct Core *core, const char *in_dirname)
{
    void *x;
    size_t dirname_length = strlen(in_dirname);
    char *dirname;
    
    dirname = malloc(dirname_length + 1);
    memcpy(dirname, in_dirname, dirname_length + 1);

    /* strip trailing slashes, if there are any */
    while (dirname_length && (dirname[dirname_length-1] == '/' || dirname[dirname_length-1] == '\\')) {
        dirname_length--;
    }

    /*
     * Start directory enumeration
     */
    x = pixie_opendir(dirname);
    if (x == NULL) {
        perror(dirname);
        free(dirname);
        return; /* no content */
    }

    /*
     * 'for all zonefiles in this directory...'
     */
    for (;;) {
        const char *filename;
        size_t original_length;

        /* Get next filename */
        filename = pixie_readdir(x);
        if (filename == NULL)
            break;
        if (strcmp(filename, ".") == 0 || strcmp(filename, "..") == 0)
            continue;

        /* Add the filename */
        original_length = core->zonefiles.length;
        conf_zonefile_addname(core, dirname, dirname_length, filename);

        /* If not ends with '.zone', ignore it */
        if (ends_with(filename, ".zone")) {
            continue;
        } else {
            core->zonefiles.length = original_length;
            core->zonefiles.total_files--;
        }



        /* if directory, recursively descend */
        {
            struct stat s;
            char *xfilename = &core->zonefiles.names[original_length];

            if (stat(xfilename, &s) == 0 && s.st_mode & S_IFDIR) {
                directory_to_zonefile_list(core, xfilename);
            }
        }



    }
    pixie_closedir(x);

    free(dirname);
}

/****************************************************************************
 ****************************************************************************/
struct XParseThread {
    struct Core *conf;
    const char *filename;
    const char *end;
    enum SuccessFailure status;
    size_t thread_handle;
    uint64_t total_bytes;
};

/****************************************************************************
 ****************************************************************************/
static void
conf_zonefiles_parse_thread(void *v)
{
    struct XParseThread *p = (struct XParseThread *)v;
    struct Core *conf = p->conf;
    struct Catalog *db = p->conf->db;
    struct ZoneFileParser *parser;
    static const struct DomainPointer root = {(const unsigned char*)"\0",1};
    const char *filename;

    LOG(1, "thread: %s begin\n", p->filename);
    fflush(stderr);
    fflush(stdout);

    /*
     * Start the parsing
     */
    LOG(1, "thread: zonefile begin\n");
    parser = zonefile_begin(
                root, 
                60, 128,
                conf->working_directory,
                zonefile_load, 
                db,
                conf->insertion_threads
                );
    LOG(1, "thread: zonefile began\n");

    
    /*
     * 'for all zonefiles in this directory...'
     */
    for (filename = p->filename; 
        filename < p->end; 
        filename += strlen(filename)+1) {
        
        FILE *fp;
        int err;
        uint64_t filesize;

        if (filename[0] == '\0')
            break;

        /*
         * Open the file
         */
        LOG(1, "thread: opening %s\n", filename);
        fflush(stdout);
        fflush(stderr);
        err = fopen_s(&fp, filename, "rb");
        if (err || fp == NULL) {
            perror(filename);
            p->status = Failure;
            return;
        }
        LOG(1, "thread: opened %s\n", filename);


        /*
         * Get the size of the file
         * TODO: there is a TOCTOU race condition here
         */
        filesize = pixie_get_filesize(filename);
        if (filesize == 0) {
            LOG(0, "%s: file is empty\n", filename);
            fclose(fp);
            continue;
        }
        p->total_bytes += filesize;
        LOG(1, "thread: %s is %u bytes\n", filename, (unsigned)filesize);


        /*
         * Set parameters
         */
        LOG(1, "thread: resetting parser\n");
        zonefile_begin_again(
            parser,
            root,   /* . domain origin */
            60,     /* one minute ttl */
            filesize, 
            filename);

        /*
         * Continue parsing the file until end, reporting progress as we
         * go along
         */
        LOG(1, "thread: parsing\n");
        for (;;) {
            unsigned char buf[65536];
            size_t bytes_read;

            bytes_read = fread((char*)buf, 1, sizeof(buf), fp);
            if (bytes_read == 0)
                break;

            zonefile_parse(
                parser,
                buf,
                bytes_read
                );

        }
        fclose(fp);
        LOG(1, "thread: parsed\n");

        //fprintf(stderr, ".");
        //fflush(stderr);
    }

    if (zonefile_end(parser) == Success) {
        //fprintf(stderr, "%s: success\n", filename);
        p->status = Success;
    } else {
        fprintf(stderr, "%s: failure\n", filename);
        p->status = Failure;
    }
    LOG(1, "thread: end\n");
}

/****************************************************************************
 ****************************************************************************/
enum SuccessFailure
conf_zonefiles_parse(   struct Catalog *db, 
                        struct Core *conf)
{
    struct XParseThread p[16];
    size_t exit_code;
    size_t thread_count = 4;
    size_t i;
    const char *names;
    enum SuccessFailure status = Success;

    LOG(1, "loading %llu zonefiles\n", conf->zonefiles.total_files);

    /*
     * Make sure we have some zonefiles to parse
     */
    if (conf->zonefiles.length == 0)
        return Failure; /* none found */

    /* The parser threads are heavy-weight, so therefore
     * we shouldn't have a lot of them unless we have
     * a lot of files to parse */
    if (conf->zonefiles.total_files < 10)
        thread_count = 1;
    else if (conf->zonefiles.total_files < 5000)
        thread_count = 2;
    else
        thread_count = 4;
    

    /*
     * Divide the list of names into equal sized chunks,
     * and launch a parsing thread for each one. The primary 
     * optimization that's happening here is that that each
     * of the threads will stall waiting for file I/O, during
     * which time other threads can be active. Each individual
     * file can be parsed with only a single thread, of course,
     * because zonefiles are stateful. However, two unrelated
     * files can be parsed at the same time.
     */
    names = conf->zonefiles.names;
    for (i=0; i<thread_count; i++) {
        const char *end;


        if (names[0] == '\0') {
            thread_count = i;
            break;
        }
        LOG(1, "loading: starting thread #%u\n", (unsigned)i);

        /*
         * Figure out the end of this chunk 
         */
        end = names + conf->zonefiles.length/thread_count;
        if (end > conf->zonefiles.length + conf->zonefiles.names)
            end = conf->zonefiles.length + conf->zonefiles.names - 2;
        while (*end)
            end++;
        end++;

        p[i].conf = conf;
        p[i].total_bytes = 0;
        p[i].filename = names;
        p[i].end = end;

        if (thread_count > 1) {
            p[i].thread_handle = pixie_begin_thread(conf_zonefiles_parse_thread, 0, &p[i]);
            LOG(1, "pthread_t: %u\n", (unsigned)p[i].thread_handle);
        } else {
            p[i].thread_handle = 0;
            conf_zonefiles_parse_thread(p);
        }
        names = end;
    }

    /*
     * Wait for them all to end
     */
    LOG(1, "loading: waiting for threads to end\n");
    for (i=0; i<thread_count; i++) {
        pixie_join(p[i].thread_handle, &exit_code);
        conf->zonefiles.total_bytes += p[i].total_bytes;
        if (p[i].status != Success)
            status = Failure;
    }
    LOG(1, "loading: threads done\n");

    return status;
}


/***************************************************************************
 ***************************************************************************/
static void conf_usage(void)
{
    printf("usage:\n");
    printf("robdns <zone-file> <conf-file> <ip-address>\n");
    exit(1);
}

/***************************************************************************
 * Echoes the configuration for one nic
 ***************************************************************************/
static void
conf_echo_nic(struct Core *conf, FILE *fp, unsigned i)
{
    char zzz[64];

    /* If we have only one adapter, then don't print the array indexes.
     * Otherwise, we need to print the array indexes to distinguish
     * the NICs from each other */
    if (conf->nic_count <= 1)
        zzz[0] = '\0';
    else
        sprintf_s(zzz, sizeof(zzz), "[%u]", i);

    fprintf(fp, "adapter%s = %s\n", zzz, conf->nic[i].ifname);
    fprintf(fp, "adapter-ip%s = %u.%u.%u.%u\n", zzz,
        (conf->nic[i].adapter_ip>>24)&0xFF,
        (conf->nic[i].adapter_ip>>16)&0xFF,
        (conf->nic[i].adapter_ip>> 8)&0xFF,
        (conf->nic[i].adapter_ip>> 0)&0xFF
        );
    fprintf(fp, "adapter-mac%s = %02x:%02x:%02x:%02x:%02x:%02x\n", zzz,
            conf->nic[i].adapter_mac[0],
            conf->nic[i].adapter_mac[1],
            conf->nic[i].adapter_mac[2],
            conf->nic[i].adapter_mac[3],
            conf->nic[i].adapter_mac[4],
            conf->nic[i].adapter_mac[5]);
    fprintf(fp, "router-mac%s = %02x:%02x:%02x:%02x:%02x:%02x\n", zzz,
            conf->nic[i].router_mac[0],
            conf->nic[i].router_mac[1],
            conf->nic[i].router_mac[2],
            conf->nic[i].router_mac[3],
            conf->nic[i].router_mac[4],
            conf->nic[i].router_mac[5]);

}

/***************************************************************************
 * Prints the current configuration to the command-line then exits.
 * Use#1: create a template file of all setable parameters.
 * Use#2: make sure your configuration was interpreted correctly.
 ***************************************************************************/
void
conf_echo(struct Core *conf, FILE *fp)
{
    unsigned i;

    fprintf(fp, "# ADAPTER SETTINGS\n");
    if (conf->nic_count == 0)
        conf_echo_nic(conf, fp, 0);
    else {
        for (i=0; i<conf->nic_count; i++)
            conf_echo_nic(conf, fp, i);
    }
}


/***************************************************************************
 ***************************************************************************/
static unsigned
hexval(char c)
{
    if ('0' <= c && c <= '9')
        return (unsigned)(c - '0');
    if ('a' <= c && c <= 'f')
        return (unsigned)(c - 'a' + 10);
    if ('A' <= c && c <= 'F')
        return (unsigned)(c - 'A' + 10);
    return 0xFF;
}

/***************************************************************************
 ***************************************************************************/
static int
parse_mac_address(const char *text, unsigned char *mac)
{
    unsigned i;

    for (i=0; i<6; i++) {
        unsigned x;
        char c;

        while (isspace(*text & 0xFF) && ispunct(*text & 0xFF))
            text++;

        c = *text;
        if (!isxdigit(c&0xFF))
            return -1;
        x = hexval(c)<<4;
        text++;

        c = *text;
        if (!isxdigit(c&0xFF))
            return -1;
        x |= hexval(c);
        text++;

        mac[i] = (unsigned char)x;

        if (ispunct(*text & 0xFF))
            text++;
    }

    return 0;
}

/***************************************************************************
 ***************************************************************************/
static uint64_t
parseInt(const char *str)
{
    uint64_t result = 0;

    while (*str && isdigit(*str & 0xFF)) {
        result = result * 10 + (*str - '0');
        str++;
    }
    return result;
}

/***************************************************************************
 * Parses the number of seconds (for rotating files mostly). We do a little
 * more than just parse an integer. We support strings like:
 *
 * hourly
 * daily
 * Week
 * 5days
 * 10-months
 * 3600
 ***************************************************************************/
uint64_t
parseTime(const char *value)
{
    uint64_t num = 0;
    unsigned is_negative = 0;

    while (*value == '-') {
        is_negative = 1;
        value++;
    }

    while (isdigit(value[0]&0xFF)) {
        num = num*10 + (value[0] - '0');
        value++;
    }
    while (ispunct(value[0]) || isspace(value[0]))
        value++;

    if (isalpha(value[0]) && num == 0)
        num = 1;

    if (value[0] == '\0')
        return num;

    switch (tolower(value[0])) {
    case 's':
        num *= 1;
        break;
    case 'm':
        num *= 60;
        break;
    case 'h':
        num *= 60*60;
        break;
    case 'd':
        num *= 24*60*60;
        break;
    case 'w':
        num *= 24*60*60*7;
        break;
    default:
        fprintf(stderr, "--rotate-offset: unknown character\n");
        exit(1);
    }
    if (num >= 24*60*60) {
        fprintf(stderr, "--rotate-offset: value is greater than 1 day\n");
        exit(1);
    }
    if (is_negative)
        num = 24*60*60 - num;

    return num;
}



/***************************************************************************
 * Tests if the named parameter on the command-line. We do a little
 * more than a straight string compare, because I get confused 
 * whether parameter have punctuation. Is it "--excludefile" or
 * "--exclude-file"? I don't know if it's got that dash. Screw it,
 * I'll just make the code so it don't care.
 ***************************************************************************/
static int
EQUALS(const char *lhs, const char *rhs)
{
    for (;;) {
        while (*lhs == '-' || *lhs == '.' || *lhs == '_')
            lhs++;
        while (*rhs == '-' || *rhs == '.' || *rhs == '_')
            rhs++;
        if (*lhs == '\0' && *rhs == '[')
            return 1; /*arrays*/
        if (tolower(*lhs & 0xFF) != tolower(*rhs & 0xFF))
            return 0;
        if (*lhs == '\0')
            return 1;
        lhs++;
        rhs++;
    }
}

static unsigned
ARRAY(const char *rhs)
{
    const char *p = strchr(rhs, '[');
    if (p == NULL)
        return 0;
    else
        p++;
    return (unsigned)parseInt(p);
}

/***************************************************************************
 * Called either from the "command-line" parser when it sees a --parm,
 * or from the "config-file" parser for normal options.
 ***************************************************************************/
void
conf_set_parameter(struct Core *conf, const char *name, const char *value)
{
    unsigned index = ARRAY(name);
    if (index >= 8) {
        fprintf(stderr, "%s: bad index\n", name);
        exit(1);
    }

    if (EQUALS("conf", name) || EQUALS("config", name)) {
        conf_read_config_file(conf, value);
    } else if (EQUALS("zonefile-benchmark", name)) {
        conf->is_zonefile_benchmark = 1;
    } else if (EQUALS("insertion-threads", name) || EQUALS("insertion-thread", name)) {
        conf->insertion_threads = (unsigned)parseInt(value);
    } else if (EQUALS("adapter", name) || EQUALS("if", name) || EQUALS("interface", name)) {
        if (conf->nic[index].ifname[0]) {
            fprintf(stderr, "CONF: overwriting \"adapter=%s\"\n", conf->nic[index].ifname);
        }
        if (conf->nic_count < index + 1)
            conf->nic_count = index + 1;
        sprintf_s(  conf->nic[index].ifname, 
                    sizeof(conf->nic[index].ifname), 
                    "%s",
                    value);

    }
    else if (EQUALS("adapter-ip", name) || EQUALS("source-ip", name) 
             || EQUALS("source-address", name) || EQUALS("spoof-ip", name)
             || EQUALS("spoof-address", name)) {
            struct ParsedIpAddress ipaddr;
            int x;

            x = parse_ip_address(value, 0, 0, &ipaddr);
            if (!x) {
                fprintf(stderr, "CONF: bad source IPv4 address: %s=%s\n", 
                        name, value);
                return;
            }

            if (ipaddr.version == 4) {
                conf->nic[index].adapter_ip = ipaddr.address[0]<<24 | ipaddr.address[1]<<16 | ipaddr.address[2]<<8 | ipaddr.address[3];
            } else {
                memcpy(conf->nic[index].adapter_ipv6, ipaddr.address, 16);
            }
    } else if (EQUALS("adapter-port", name) || EQUALS("source-port", name)) {
        /* Send packets FROM this port number */
        unsigned x = strtoul(value, 0, 0);
        if (x > 65535) {
            fprintf(stderr, "error: %s=<n>: expected number less than 1000\n", 
                    name);
        } else {
            conf->nic[index].adapter_port = x;
        }
    } else if (EQUALS("adapter-mac", name) || EQUALS("spoof-mac", name)
               || EQUALS("source-mac", name)) {
        /* Send packets FROM this MAC address */
        unsigned char mac[6];

        if (parse_mac_address(value, mac) != 0) {
            fprintf(stderr, "CONF: bad MAC address: %s=%s\n", name, value);
            return;
        }

        memcpy(conf->nic[index].adapter_mac, mac, 6);
    }
    else if (EQUALS("router-mac", name) || EQUALS("router", name)) {
        unsigned char mac[6];

        if (parse_mac_address(value, mac) != 0) {
            fprintf(stderr, "CONF: bad MAC address: %s=%s\n", name, value);
            return;
        }

        memcpy(conf->nic[index].router_mac, mac, 6);
    } else {
        fprintf(stderr, "CONF: unknown config option: %s=%s\n", name, value);
    }
}


void
conf_help()
{
    printf("TODO: this feature (providing help) not yet implemented\n");
    exit(1);
}

/***************************************************************************
 * Tests if the command-line option is a directory, in which case, we
 * need to read configuration files and zone-files from that directory
 ***************************************************************************/
static int
is_directory(const char *filename)
{
    struct stat s;

    if (stat(filename, &s) != 0)
        return 0; /* bad filenames not directories */

    return (s.st_mode & S_IFDIR) > 0;
}




/***************************************************************************
 ***************************************************************************/
static int
has_configuration(const char *dirname)
{
    void *x;
    int is_found = 0;

    x = pixie_opendir(dirname);
    if (x == NULL)
        return 0; /* no content */

    for (;;) {
        const char *filename;
        
        filename = pixie_readdir(x);
        if (filename == NULL)
            break;

        if (strcmp(filename, ".") == 0 || strcmp(filename, "..") == 0)
            continue;

        if (ends_with(filename, ".zone") || ends_with(filename, ".conf")) {
            is_found = 1;
            break;
        }

        {
            char *xdirname = combine_filename(dirname, filename);

            if (is_directory(xdirname))
                is_found = has_configuration(xdirname);

            free(xdirname);

            if (is_found)
                break;
        }
    }


    pixie_closedir(x);
    return is_found;
}


/***************************************************************************
 * Read the configuration from the command-line.
 * Called by 'main()' when starting up.
 ***************************************************************************/
void
conf_command_line(struct Core *conf, int argc, char *argv[])
{
    int i;
    struct ParsedIpAddress ipaddr;

    for (i=1; i<argc; i++) {

        /*
         * --name=value
         * --name:value
         * -- name value
         */
        if (argv[i][0] == '-' && argv[i][1] == '-') {
            if (strcmp(argv[i], "--help") == 0)
                conf_help();
            else {
                char name2[64];
                char *name = argv[i] + 2;
                unsigned name_length;
                const char *value;

                value = strchr(&argv[i][2], '=');
                if (value == NULL)
                    value = strchr(&argv[i][2], ':');
                if (value == NULL) {
                    value = argv[++i];
                    name_length = (unsigned)strlen(name);
                } else {
                    name_length = (unsigned)(value - name);
                    value++;
                }

                if (i >= argc) {
                    fprintf(stderr, "%.*s: empty parameter\n", name_length, name);
                    break;
                }

                if (name_length > sizeof(name2) - 1) {
                    fprintf(stderr, "%.*s: name too long\n", name_length, name);
                    name_length = sizeof(name2) - 1;
                }

                memcpy(name2, name, name_length);
                name2[name_length] = '\0';

                conf_set_parameter(conf, name2, value);
            }
            continue;
        }

        /* For for a single-dash parameter */
        else if (argv[i][0] == '-') {
            const char *arg;

            switch (argv[i][1]) {
            case 'i':
                if (argv[i][2])
                    arg = argv[i]+2;
                else
                    arg = argv[++i];
                conf_set_parameter(conf, "adapter", arg);
                break;
            case 'h':
            case '?':
                conf_usage();
                break;
            case 'v':
                verbosity++;
                break;
            default:
                LOG(0, "FAIL: unknown option: -%s\n", argv[i]);
                LOG(0, " [hint] try \"--help\"\n");
                LOG(0, " [hint] ...or, to list nmap-compatible options, try \"--nmap\"\n");
                exit(1);
            }
            continue;
        }
        else if (ends_with(argv[i], ".zone"))
            conf_zonefile_addname(conf, conf->working_directory, strlen(conf->working_directory), argv[i]);
        else if (parse_ip_address(argv[i], 0, 0, &ipaddr)) {
            conf_set_parameter(conf, "adapter-ip", argv[i]);
        } else if (pixie_nic_exists(argv[i])) {
            strcpy_s(conf->nic[0].ifname, sizeof(conf->nic[0].ifname), argv[i]);
        } else if (is_directory(argv[i]) && has_configuration(argv[i])) {
            directory_to_zonefile_list(conf, argv[i]);
        } else {
            LOG(0, "%s: unknown command-line parameter\n", argv[i]);
        }

    }
}

/***************************************************************************
 * remove leading/trailing whitespace
 ***************************************************************************/
static void
trim(char *line)
{
    while (isspace(*line & 0xFF))
        memmove(line, line+1, strlen(line));
    while (isspace(line[strlen(line)-1] & 0xFF))
        line[strlen(line)-1] = '\0';
}

/***************************************************************************
 ***************************************************************************/
void
conf_read_config_file(struct Core *conf, const char *filename)
{
    FILE *fp;
    errno_t err;
    char line[65536];

    err = fopen_s(&fp, filename, "rt");
    if (err) {
        perror(filename);
        return;
    }

    while (fgets(line, sizeof(line), fp)) {
        char *name;
        char *value;

        trim(line);

        if (ispunct(line[0] & 0xFF) || line[0] == '\0')
            continue;

        name = line;
        value = strchr(line, '=');
        if (value == NULL)
            continue;
        *value = '\0';
        value++;
        trim(name);
        trim(value);

        conf_set_parameter(conf, name, value);
    }

    fclose(fp);
}

/***************************************************************************
 ***************************************************************************/
void 
conf_init(struct Core *core)
{
    memset(core, 0, sizeof(*core));

    /* Get the current working directory, because on various debuggers,
     * like XCode and VisualStudio, I get confused where the current
     * directory is located */
    getcwd(core->working_directory, sizeof(core->working_directory));
    LOG(0, "cwd: %s\n", core->working_directory);

    /* Initialize the list of zonefiles. In a "hosting" environment, this
     * can get up to a million files.
     * CODE NOTE: we set it to 2 bytes, which is always insufficient, so
     * that the codepath that auto-extends it is forced to be executed
     * in all cases, rather than just a test case. */
    core->zonefiles.max = 2;
    core->zonefiles.names = malloc(core->zonefiles.max + 2);
}

