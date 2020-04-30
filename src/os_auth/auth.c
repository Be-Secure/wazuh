/* Auth Common
 * Copyright (C) 2015-2020, Wazuh Inc.
 * Mar 22, 2018.
 *
 * This program is free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation.
 */

#include <shared.h>
#include "auth.h"
#include "os_err.h"

#ifdef UNIT_TESTING
#define static
#endif

/*authd working as worker on cluster*/
bool worker_node;

keystore keys;
char shost[512];
authd_config_t config;

struct keynode *queue_insert = NULL;
struct keynode *queue_backup = NULL;
struct keynode *queue_remove = NULL;
struct keynode * volatile *insert_tail;
struct keynode * volatile *backup_tail;
struct keynode * volatile *remove_tail;

// Append key to insertion queue
void add_insert(const keyentry *entry,const char *group) {
    struct keynode *node;

    os_calloc(1, sizeof(struct keynode), node);
    node->id = strdup(entry->id);
    node->name = strdup(entry->name);
    node->ip = strdup(entry->ip->ip);
    node->group = NULL;

    if(group != NULL)
        node->group = strdup(group);

    (*insert_tail) = node;
    insert_tail = &node->next;
}

// Append key to backup queue
void add_backup(const keyentry *entry) {
    struct keynode *node;

    os_calloc(1, sizeof(struct keynode), node);
    node->id = strdup(entry->id);
    node->name = strdup(entry->name);
    node->ip = strdup(entry->ip->ip);

    (*backup_tail) = node;
    backup_tail = &node->next;
}

// Append key to deletion queue
void add_remove(const keyentry *entry) {
    struct keynode *node;

    os_calloc(1, sizeof(struct keynode), node);
    node->id = strdup(entry->id);
    node->name = strdup(entry->name);
    node->ip = strdup(entry->ip->ip);

    (*remove_tail) = node;
    remove_tail = &node->next;
}


w_err_t w_auth_parse_data(const char* buf, char *response,const char *authpass, char *ip, char **agentname, char **groups){
    
    bool parseok = FALSE;     
    /* Checking for shared password authentication. */
    if(authpass) {
        /* Format is pretty simple: OSSEC PASS: PASS WHATEVERACTION */
        parseok = FALSE; 
        if (strncmp(buf, "OSSEC PASS: ", 12) == 0) {
            buf += 12;
            if (strlen(buf) > strlen(authpass) && strncmp(buf, authpass, strlen(authpass)) == 0) {
                buf += strlen(authpass);
                if (*buf == ' ') {
                    buf++;
                    parseok = 1;
                }
            }
        }

        if (parseok == 0) {
            merror("Invalid password provided by %s. Closing connection.", ip);
            snprintf(response, 2048, "ERROR: Invalid password\n\n");  
            return OS_INVALID;
        }
    }

    /* Checking for action A (add agent) */    
    parseok = FALSE;
    if (strncmp(buf, "OSSEC A:'", 9) == 0) {        
        buf += 9;
                
        unsigned len = 0;
        while (*buf != '\0') {
            if (*buf == '\'') {
                os_malloc(len+1, *agentname);
                memcpy(*agentname, buf-len, len);
                (*agentname)[len] = '\0';
                minfo("Received request for a new agent (%s) from: %s", *agentname, ip);
                parseok = TRUE;
                break;
            }
            len++; 
            buf++;           
        }
    }
    buf++;    
    
    if (!parseok) {
        merror("Invalid request for new agent from: %s", ip);
        snprintf(response, 2048, "ERROR: Invalid request for new agent\n\n");  
        return OS_INVALID;
    }    

    if (!OS_IsValidName(*agentname)) {
        merror("Invalid agent name: %s from %s", *agentname, ip);
        snprintf(response, 2048, "ERROR: Invalid agent name: %s\n\n", *agentname);  
        return OS_INVALID;
    }

    /* Check for valid centralized group */
    
    char centralized_group_token[2] = "G:";

    if(strncmp(++buf,centralized_group_token,2)==0)
    {
        char tmp_groups[OS_SIZE_65536] = {0};
        sscanf(buf," G:\'%65535[^\']\"",tmp_groups);

        /* Validate the group name */
        int valid = 0;
        valid = w_validate_group_name(tmp_groups);

        if(valid < 0) {

            merror("Invalid group name: %.255s... ,",tmp_groups);

            switch (valid) {
                case -6:
                    snprintf(response, 2048, "ERROR: Invalid group name: %.255s... cannot start or end with ','\n\n", tmp_groups);
                    break;
                case -5:
                    snprintf(response, 2048, "ERROR: Invalid group name: %.255s... consecutive ',' are not allowed \n\n, ", tmp_groups);
                    break;
                case -4:
                    snprintf(response, 2048, "ERROR: Invalid group name: %.255s... white spaces are not allowed \n\n", tmp_groups);
                    break;
                case -3:
                    snprintf(response, 2048, "ERROR: Invalid group name: %.255s... multigroup is too large \n\n", tmp_groups);
                    break;
                case -2:
                    snprintf(response, 2048, "ERROR: Invalid group name: %.255s... group is too large\n\n", tmp_groups);
                    break;
                case -1:
                    snprintf(response, 2048, "ERROR: Invalid group name: %.255s... characters '\\/:*?\"<>|,' are prohibited\n\n", tmp_groups);
                    break;
            }               
            return OS_INVALID;
        }
        *groups = wstr_delete_repeated_groups(tmp_groups);
        if(!*groups){
            return OS_MEMERR;
        }
        mdebug1("Group(s) is: %s",*groups);
        
        /*Forward the string pointer G:'........' 2 for G:, 2 for ''*/
        buf+= 2+strlen(tmp_groups)+2;
        
    }else{
        buf--;
    }

    /* Check for IP when client uses -i option */
    
    char client_source_ip[IPSIZE + 1] = {0};
    char client_source_ip_token[3] = "IP:";
    
    if(strncmp(++buf,client_source_ip_token,3)==0) {
        char format[15];
        sprintf(format, " IP:\'%%%d[^\']\"", IPSIZE);
        sscanf(buf, format ,client_source_ip);

        /* If IP: != 'src' overwrite the provided ip */
        if(strncmp(client_source_ip,"src",3) != 0)
        {
            if (!OS_IsValidIP(client_source_ip, NULL)) {
                merror("Invalid IP: '%s'", client_source_ip);
                snprintf(response, 2048, "ERROR: Invalid IP: %s\n\n", client_source_ip);                    
                return OS_INVALID;
            }
            snprintf(ip, IPSIZE, "%s", client_source_ip);
        }
        
    } 
    else if(!config.flags.use_source_ip) {
        // use_source-ip = 0 and no -I argument in agent
        snprintf(ip, IPSIZE, "any");
    }
    // else -> agent IP is already on ip         
   
    return OS_SUCCESS;
}

w_err_t w_auth_validate_data (char *response, const char *ip, const char *agentname, const char *groups){
    /* Validate the group(s) name(s) */     
    int index = 0;
    char *id_exist = NULL;
    double antiquity = 0;
    if (groups){ 
        if (OS_SUCCESS != w_auth_validate_groups(groups, response)){
            return OS_INVALID;
        }        
    }

    /* Check for duplicated IP */
    if (strcmp(ip, "any") != 0 ) {
        if (index = OS_IsAllowedIP(&keys, ip), index >= 0) {
            if (config.flags.force_insert && (antiquity = OS_AgentAntiquity(keys.keyentries[index]->name, keys.keyentries[index]->ip->ip), antiquity >= config.force_time || antiquity < 0)) {
                id_exist = keys.keyentries[index]->id;
                minfo("Duplicated IP '%s' (%s). Saving backup.", ip, id_exist);
                
                OS_RemoveAgentGroup(id_exist);
                add_backup(keys.keyentries[index]);
                OS_DeleteKey(&keys, id_exist, 0);
            } else {
                merror("Duplicated IP %s", ip);
                snprintf(response, 2048, "ERROR: Duplicated IP: %s\n\n", ip);
                return OS_INVALID;
            }
        }
    }
    
    /* Check whether the agent name is the same as the manager */

    if (!strcmp(agentname, shost)) {
        merror("Invalid agent name %s (same as manager)", agentname);
        snprintf(response, 2048, "ERROR: Invalid agent name: %s\n\n", agentname);
        return OS_INVALID;
    }

    /* Check for duplicated names */

    if (index = OS_IsAllowedName(&keys, agentname), index >= 0) {
        if (config.flags.force_insert && (antiquity = OS_AgentAntiquity(keys.keyentries[index]->name, keys.keyentries[index]->ip->ip), antiquity >= config.force_time || antiquity < 0)) {
            id_exist = keys.keyentries[index]->id;
            minfo("Duplicated name '%s' (%s). Saving backup.", agentname, id_exist);

            add_backup(keys.keyentries[index]);
            OS_DeleteKey(&keys, id_exist, 0);
        } else {
            merror("Invalid agent name %s (duplicated)", agentname);
            snprintf(response, 2048, "ERROR: Duplicated agent name: %s\n\n", agentname);                
            return OS_INVALID;
        }
    }

    /* Check for agents limit */

    if (config.flags.register_limit && keys.keysize >= (MAX_AGENTS - 2) ) {
        merror(AG_MAX_ERROR, MAX_AGENTS - 2);
        snprintf(response, 2048, "ERROR: The maximum number of agents has been reached\n\n");
        return OS_INVALID;
    }

    return OS_SUCCESS;
}

w_err_t w_auth_add_agent(char *response, const char *ip, const char *agentname, const char *groups, char **id, char **key){

    /* Add the new agent */
    int index;

    if (index = OS_AddNewAgent(&keys, NULL, agentname, ip, NULL), index < 0) {
        merror("Unable to add agent: %s (internal error)", agentname);
        snprintf(response, 2048, "ERROR: Internal manager error adding agent: %s\n\n", agentname);        
        return OS_INVALID;
    }

    /* Add the agent to the centralized configuration group */
    if(groups) {
        char path[PATH_MAX];
        if (snprintf(path, PATH_MAX, isChroot() ? GROUPS_DIR "/%s" : DEFAULTDIR GROUPS_DIR "/%s", keys.keyentries[index]->id) >= PATH_MAX) {
            merror("At set_agent_group(): file path too large for agent '%s'.", keys.keyentries[index]->id);
            OS_RemoveAgent(keys.keyentries[index]->id);
            merror("Unable to set agent centralized group: %s (internal error)", groups);
            snprintf(response, 2048, "ERROR: Internal manager error setting agent centralized group: %s\n\n", groups);
            return OS_INVALID;
        }
    }

    os_strdup(keys.keyentries[index]->id, *id);
    os_strdup(keys.keyentries[index]->key, *key);

    return OS_SUCCESS;
}

w_err_t w_auth_validate_groups(const char *groups, char *response) {
    int max_multigroups = 0;    
    char *save_ptr = NULL;
    char *tmp_groups = NULL;
    const char delim[] = {MULTIGROUP_SEPARATOR,'\0'};    
  
    os_strdup(groups, tmp_groups);
    char *group = strtok_r(tmp_groups, delim, &save_ptr);    

    while( group != NULL ) {
        DIR * dp;
        char dir[PATH_MAX + 1] = {0};        

        /* Check limit */
        if(max_multigroups > MAX_GROUPS_PER_MULTIGROUP){
            merror("Maximum multigroup reached: Limit is %d",MAX_GROUPS_PER_MULTIGROUP);     
            if (response) {
                snprintf(response, 2048, "ERROR: Maximum multigroup reached: Limit is %d\n\n", MAX_GROUPS_PER_MULTIGROUP); 
            }             
            return OS_INVALID;
        }
        /* Validate the group name */
        if( 0 != w_validate_group_name(group) ){ 
            merror("Invalid group name: %.255s... ,",group); 
            if (response){
                snprintf(response, 2048, "ERROR: Invalid group name: %.255s... group is too large\n\n", group); 
            }                                   
            return OS_INVALID;
        }       

        snprintf(dir, PATH_MAX + 1,isChroot() ? SHAREDCFG_DIR"/%s" : DEFAULTDIR SHAREDCFG_DIR"/%s", group);
        dp = opendir(dir);
        if (!dp) {            
            merror("Invalid group: %.255s",group);   
            if (response){
                snprintf(response, 2048, "ERROR: Invalid group: %s\n\n", group);
            }             
            return OS_INVALID;
        }

        group = strtok_r(NULL, delim, &save_ptr);
        max_multigroups++;
        closedir(dp);
    }       
   
    return OS_SUCCESS;
}


