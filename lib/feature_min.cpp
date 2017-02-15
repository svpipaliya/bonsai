#include <cctype>
#include "feature_min.h"

namespace emp {


void lca2depth(khash_t(c) *lca_map, khash_t(p) *tax_map) {
    for(khiter_t ki(kh_begin(lca_map)); ki != kh_end(lca_map); ++ki)
        if(kh_exist(lca_map, ki))
            kh_val(lca_map, ki) = node_depth(tax_map, kh_val(lca_map, ki));
}

khash_t(c) *make_depth_hash(khash_t(c) *lca_map, khash_t(p) *tax_map) {
    khash_t(c) *ret(kh_init(c));
    kh_resize(c, ret, kh_size(lca_map));
    khiter_t ki1;
    int khr;
    for(khiter_t ki2(kh_begin(lca_map)); ki2 != kh_end(lca_map); ++ki2) {
        if(kh_exist(lca_map, ki2)) {
            ki1 = kh_put(c, ret, kh_key(lca_map, ki2), &khr);
            kh_val(ret, ki1) = node_depth(tax_map, kh_val(lca_map, ki2));
        }
    }
    return ret;
}

#if !NDEBUG
#define _DBP(h) LOG_DEBUG("hash map %s is at %p.\n", #h, (void *)h)
#else
#define _DBP(h)
#endif

void update_minimized_map(khash_t(all) *set, khash_t(64) *full_map, khash_t(c) *ret, int mode) {
    _DBP(set);
    _DBP(full_map);
    _DBP(ret);
    int khr;
    khiter_t kif, kir;
    if(!mode) LOG_EXIT("Mode %i in score scheme is not good.\n");
    LOG_DEBUG("Size of set: %zu\n", kh_size(set));
    for(khiter_t ki(0); ki != kh_end(set); ++ki) {
        if(!kh_exist(set, ki) ||
               kh_get(c, ret, kh_key(set, ki)) != kh_end(ret))
            continue;
            // If the key is already in the main map, what's the problem?
        if(unlikely((kif = kh_get(64, full_map, kh_key(set, ki))) == kh_end(full_map)))
            LOG_EXIT("Missing kmer from database... Check for matching spacer and kmer size.\n");
        kir = kh_put(c, ret, kh_key(full_map, kif), &khr);
        kh_val(ret, kir) = kh_val(full_map, kif);
        if(unlikely(kh_size(ret) % 1000000 == 0)) LOG_INFO("Final hash size %zu\n", kh_size(ret));
    }
    return;
}

#if !NDEBUG
#undef _DBP
#endif

khash_t(64) *make_taxdepth_hash(khash_t(c) *kc, khash_t(p) *tax) {
    khash_t(64) *ret(kh_init(64));
    int khr;
    khiter_t kir;
    kh_resize(64, ret, kc->n_buckets);
    for(khiter_t ki(0); ki != kh_end(kc); ++ki) {
        if(kh_exist(kc, ki)) {
            kir = kh_put(64, ret, kh_key(kc, ki), &khr);
            kh_val(ret, kir) = TDencode(node_depth(tax, kh_val(kc, ki)), kh_val(kc, ki));
        }
    }
    return ret;
}

void update_td_map(khash_t(64) *kc, khash_t(all) *set, khash_t(p) *tax, std::uint32_t taxid) {
    int khr;
    khint_t k2;
    std::uint32_t val;
    LOG_DEBUG("Adding set of size %zu t total set of current size %zu.\n", kh_size(set), kh_size(kc));
    for(khiter_t ki(kh_begin(set)); ki != kh_end(set); ++ki) {
        if(kh_exist(set, ki)) {
            if((k2 = kh_get(64, kc, kh_key(set, ki))) == kh_end(kc)) {
                k2 = kh_put(64, kc, kh_key(set, ki), &khr);
                kh_val(kc, k2) = TDencode(node_depth(tax, kh_val(kc, ki)), kh_val(kc, ki));
#if !NDEBUG
                if(unlikely(kh_size(kc) % 1000000 == 0)) LOG_INFO("Final hash size %zu\n", kh_size(kc));
#endif
            } else if(kh_val(kc, k2) != taxid) {
                val = lca(tax, taxid, kh_val(kc, k2));
                if(val == (std::uint32_t)-1) {
                    kh_val(kc, k2) = 1;
                    LOG_WARNING("Missing taxid %u. Setting lca to tree root\n", taxid);
                } else kh_val(kc, k2) = TDencode(node_depth(tax, val), val);
            }
        }
    }
    LOG_DEBUG("After updating with set of size %zu, total set current size is %zu.\n", kh_size(set), kh_size(kc));
}


void update_lca_map(khash_t(c) *kc, khash_t(all) *set, khash_t(p) *tax, std::uint32_t taxid) {
    int khr;
    khint_t k2;
    std::uint32_t val;
    LOG_DEBUG("Adding set of size %zu t total set of current size %zu.\n", kh_size(set), kh_size(kc));
    for(khiter_t ki(kh_begin(set)); ki != kh_end(set); ++ki) {
        if(kh_exist(set, ki)) {
            if((k2 = kh_get(c, kc, kh_key(set, ki))) == kh_end(kc)) {
                k2 = kh_put(c, kc, kh_key(set, ki), &khr);
                kh_val(kc, k2) = taxid;
                if(unlikely(kh_size(kc) % 1000000 == 0)) LOG_INFO("Final hash size %zu\n", kh_size(kc));
            } else if(kh_val(kc, k2) != taxid) {
                if(unlikely((val = lca(tax, taxid, kh_val(kc, k2))) == UINT32_C(-1))) {
                    kh_val(kc, k2) = 1;
                    LOG_WARNING("Missing taxid %u. Setting lca \n", taxid);
                } else kh_val(kc, k2) = val;
            }
        }
    }
    LOG_DEBUG("After updating with set of size %zu, total set current size is %zu.\n", kh_size(set), kh_size(kc));
}

std::uint32_t get_taxid(const char *fn, khash_t(name) *name_hash) {
    gzFile fp(gzopen(fn, "rb"));
    if(fp == nullptr) LOG_EXIT("Could not read from file %s\n", fn);
    static const std::size_t bufsz(2048);
    khint_t ki;
    char buf[bufsz];
    char *line(gzgets(fp, buf, bufsz));
    char *p(++line);
    if(strchr(p, '|')) {
        // Handle old refseq.
        p = strchr(p, '|');
        p = strchr(p, '|');
        p = strchr(p, '|') + 1; // Now p is at the accession.
        char *q(strchr(p, '|'));
        if(q == nullptr) LOG_EXIT("Malformed line %s", buf);
        *q = 0;
        line = p;
    } else {
        while(!std::isspace(*p)) ++p;
        *p = 0;
    }
    if(unlikely((ki = kh_get(name, name_hash, line)) == kh_end(name_hash))) {
        fprintf(stderr, "Missing taxid for %s.\n", line);
        std::exit(EXIT_FAILURE);
    }
    gzclose(fp);
    return kh_val(name_hash, ki);
}


} //namespace emp
