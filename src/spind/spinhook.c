#include <assert.h>

#include "core2pubsub.h"
#include "spind.h"
#include "spin_log.h"
#include "statistics.h"

STAT_MODULE(spinhook)

devflow_t *spinhook_get_devflow(device_t *dev, node_t *node) {
    tree_entry_t *leaf;
    int nodeid = node->id;
    devflow_t *dfp;
    int *newint;
    STAT_COUNTER(ctr, traffic, STAT_TOTAL);

    leaf = tree_find(dev->dv_flowtree, sizeof(nodeid), &nodeid);
    STAT_VALUE(ctr, leaf!=NULL);
    if (leaf == NULL) {
        spin_log(LOG_DEBUG, "Create new devflow_t\n");
        dfp = malloc(sizeof(devflow_t));
        dfp->dvf_blocked = 0;
        dfp->dvf_packets = 0;
        dfp->dvf_bytes = 0;
        dfp->dvf_lastseen = 0;
        dfp->dvf_idleperiods = 0;
        dfp->dvf_activelastperiod = 0;

        newint = malloc(sizeof(int));
        *newint = nodeid;
        // Add flow record indexed by destination nodeid
        // Own the storage here
        tree_add(dev->dv_flowtree, sizeof(int), newint, sizeof(devflow_t), dfp, 0);
        dev->dv_nflows++;
        // Increase node reference count
        node->references++;
    } else {
        dfp = (devflow_t *) leaf->data;
    }
    return dfp;
}

void
spinhook_block_dev_node_flow(device_t *dev, node_t *node, int blocked) {
    devflow_t *dfp;

    dfp = spinhook_get_devflow(dev, node);
    dfp->dvf_blocked = blocked;
}

void
do_traffic(device_t *dev, node_t *node, int cnt, int bytes, uint32_t timestamp) {
    devflow_t *dfp;

    dfp = spinhook_get_devflow(dev, node);
    dfp->dvf_packets += cnt;
    dfp->dvf_bytes += bytes;
    dfp->dvf_lastseen = timestamp;
    dfp->dvf_activelastperiod = 1;
}

// Checks if there is a node with the given mac already, and if so,
// merge the two nodes, and return the pointer of the existing node
// If not, set the mac, and return the node pointer itself
static node_t*
check_for_existing_node_with_mac(node_cache_t* node_cache, node_t* node, char* mac) {
    node_t* result_node, * existing_node;
    // If there is a node with this mac already, merge this one into it,
    existing_node = node_cache_find_by_mac(node_cache, mac);
    if (existing_node != NULL) {
        merge_nodes(node_cache, node, existing_node);
        // use the existing node now in this function
        result_node = existing_node;
    } else {
        node_set_mac(node, mac);
        cache_tree_remove_mac(node_cache, mac);
        cache_tree_add_mac(node_cache, node, mac);
        result_node = node;
    }
    // If it is not considered a device yet, make it one now
    if (result_node->device == NULL) {
        makedevice(result_node);
    }
    return result_node;
}

void
spinhook_traffic(node_cache_t *node_cache, node_t *src_node, node_t *dest_node, int packetcnt, int packetbytes, uint32_t timestamp) {
    int found = 0;
    char *updated_mac = NULL;
    tree_entry_t* node_ip = NULL;

    if (src_node == dest_node) {
        // Probably internal stuff
        return;
    }
    if (!src_node->device && !dest_node->device) {
        // neither are known to be a device;
        // this may indicate we have outdated ARP information
        // Update it, and check again for both nodes
        spin_log(LOG_DEBUG, "No device in %d to %d traffic\n", src_node->id, dest_node->id);
        node_cache_update_arp(node_cache, timestamp);
        node_ip = tree_first(src_node->ips);
        while (node_ip) {
            updated_mac = arp_table_find_by_ip(node_cache->arp_table, node_ip->key);
            if (updated_mac) {
                src_node = check_for_existing_node_with_mac(node_cache, src_node, updated_mac);
            }
            node_ip = tree_next(node_ip);
        }
        node_ip = tree_first(dest_node->ips);
        while (node_ip) {
            updated_mac = arp_table_find_by_ip(node_cache->arp_table, node_ip->key);
            if (updated_mac) {
                src_node = check_for_existing_node_with_mac(node_cache, dest_node, updated_mac);
            }
            node_ip = tree_next(node_ip);
        }
    }

    if (src_node->device) {
        do_traffic(src_node->device, dest_node, packetcnt, packetbytes, timestamp);
        found++;
    }
    if (dest_node->device) {
        do_traffic(dest_node->device, src_node, packetcnt, packetbytes, timestamp);
        found++;
    }
    // TODO: do we need to check yet again?
    if (!found) {
        spin_log(LOG_DEBUG, "STILL No device in %d to %d traffic\n", src_node->id, dest_node->id);

        // Probably ARP cache must be reread and acted upon here
        node_cache_update_arp(node_cache, timestamp);
    }
}

static void
device_flow_remove(node_cache_t *node_cache, tree_t *ftree, tree_entry_t* leaf) {
    node_t *remnode;
    int remnodenum;
    STAT_COUNTER(ctr, flow-remove, STAT_TOTAL);

    remnodenum = * (int *) leaf->key;
    spin_log(LOG_DEBUG, "Remove flow to %d\n", remnodenum);

    remnode = node_cache_find_by_id(node_cache, remnodenum);
    assert(remnode != 0);
    remnode->references--;

    // This also frees key and data
    tree_remove_entry(ftree, leaf);

    STAT_VALUE(ctr, 1);
}

#define MAX_IDLE_PERIODS    10
#define MIN_DEV_NEIGHBOURS  5

static void
device_clean(node_cache_t *node_cache, node_t *node, void *ap) {
    device_t *dev;
    tree_entry_t *leaf, *nextleaf;
    int remnodenum;
    devflow_t *dfp;
    int removed = 0;
    STAT_COUNTER(ctr, device-clean, STAT_TOTAL);

    dev = node->device;
    assert(dev != NULL);

    spin_log(LOG_DEBUG, "Flows(%d) of node %d:\n", dev->dv_nflows, node->id);
    leaf = tree_first(dev->dv_flowtree);
    while (leaf != NULL) {
        nextleaf = tree_next(leaf);
        remnodenum = * (int *) leaf->key;
        dfp = (devflow_t *) leaf->data;
        spin_log(LOG_DEBUG, "to node %d: %d %d %d %d\n", remnodenum,
            dfp->dvf_packets, dfp->dvf_bytes,
            dfp->dvf_idleperiods, dfp->dvf_activelastperiod);

        if (dfp->dvf_activelastperiod) {
            dfp->dvf_idleperiods = -1;
            dfp->dvf_activelastperiod = 0;
        }

        dfp->dvf_idleperiods++;
        if (dev->dv_nflows > MIN_DEV_NEIGHBOURS && dfp->dvf_idleperiods > MAX_IDLE_PERIODS) {
            device_flow_remove(node_cache, dev->dv_flowtree, leaf);
            dev->dv_nflows--;
            removed++;
        }

        leaf = nextleaf;
    }

    STAT_VALUE(ctr, removed);
}

void
spinhook_clean(node_cache_t *node_cache) {

    node_callback_devices(node_cache, device_clean, NULL);
}

static void
node_merge_flow(node_cache_t *node_cache, node_t *node, void *ap) {
    device_t *dev;
    tree_entry_t *srcleaf, *dstleaf;
    int *remnodenump;
    devflow_t *dfp, *destdfp;
    node_t *src_node, *dest_node;
    STAT_COUNTER(ctr, merge-flow, STAT_TOTAL);
    int *nodenumbers = (int *) ap;
    int srcnodenum, dstnodenum;

    dev = node->device;
    assert(dev != NULL);

    srcnodenum = nodenumbers[0];
    dstnodenum = nodenumbers[1];

    spin_log(LOG_DEBUG, "Renumber %d->%d in flows(%d) of node %d:\n", srcnodenum, dstnodenum, dev->dv_nflows, node->id);

    srcleaf = tree_find(dev->dv_flowtree, sizeof(srcnodenum), &srcnodenum);

    STAT_VALUE(ctr, srcleaf!= NULL);

    if (srcleaf == NULL) {
        // Nothing do to  here
        return;
    }

    // This flow must be renumbered

    // Merge these two flow numbers if destination also in flowlist
    dstleaf = tree_find(dev->dv_flowtree, sizeof(dstnodenum), &dstnodenum);

    src_node = node_cache_find_by_id(node_cache, srcnodenum);
    assert(src_node != NULL);
    assert(src_node->references > 0);

    remnodenump = (int *) srcleaf->key;

    dfp = (devflow_t *) srcleaf->data;
    srcleaf->key = NULL;
    srcleaf->data = NULL;
    tree_remove_entry(dev->dv_flowtree, srcleaf);

    if (dstleaf != 0) {
        // Merge the numbers
        destdfp = (devflow_t *) dstleaf->data;
        destdfp->dvf_packets += dfp->dvf_packets;;
        destdfp->dvf_bytes += dfp->dvf_bytes;
        destdfp->dvf_idleperiods = 0;
        destdfp->dvf_activelastperiod = 1;

        free(remnodenump);
        free(dfp);

        dev->dv_nflows--;
    } else {
        // Reuse memory of key and data, renumber key
        dest_node = node_cache_find_by_id(node_cache, dstnodenum);
        assert(dest_node != NULL);

        *remnodenump = dstnodenum;
        tree_add(dev->dv_flowtree, sizeof(int), remnodenump, sizeof(devflow_t), dfp, 0);
        spin_log(LOG_DEBUG, "Added new leaf\n");
        dest_node->references++;
    }
    src_node->references--;
}

void
flows_merged(node_cache_t *node_cache, int node1, int node2) {
    int nodenumbers[2];

    nodenumbers[0] = node1;
    nodenumbers[1] = node2;
    node_callback_devices(node_cache, node_merge_flow, (void *) nodenumbers);
}

static char *adminchannel = "SPIN/traffic/admin";

void
spinhook_nodedeleted(node_cache_t *node_cache, node_t *node) {
    spin_data command;
    spin_data sd;
    char mosqchan[100];

    publish_nodes();
    sd = spin_data_node_deleted(node->id);

    command = spin_data_create_mqtt_command("nodeDeleted", NULL, sd);
    core2pubsub_publish_chan(adminchannel, command, 0);
    spin_data_delete(command);

    sprintf(mosqchan, "SPIN/traffic/node/%d", node->id);
    core2pubsub_publish_chan(mosqchan, NULL, 1);
}

void
spinhook_nodesmerged(node_cache_t *node_cache, node_t *dest_node, node_t *src_node) {
    spin_data command;
    spin_data sd;
    char mosqchan[100];

    publish_nodes();
    sd = spin_data_nodes_merged(src_node->id, dest_node->id);

    command = spin_data_create_mqtt_command("nodeMerged", NULL, sd);
    core2pubsub_publish_chan(adminchannel, command, 0);
    spin_data_delete(command);

    sprintf(mosqchan, "SPIN/traffic/node/%d", src_node->id);
    core2pubsub_publish_chan(mosqchan, NULL, 1);

    flows_merged(node_cache, src_node->id, dest_node->id);
}
