#ifndef _FEATURE_MIN__
#define _FEATURE_MIN__

#include "encoder.h"
#include "spacer.h"
#include "khash64.h"
#include "util.h"

#include <set>

// Decode 64-bit hash (contains both tax id and taxonomy depth for id)
#define TDtax(key) ((std::uint32_t)key)
#define TDdepth(key) ((std::uint32_t)~0 - (key >> 32))
#define TDencode(depth, taxid) (((std::uint64_t)((std::uint32_t)~0 - depth) << 32) | taxid)

// Decode 64-bit hash for feature counting.
// TODO: add building of FeatureMin hash
#define FMtax(key) ((std::uint32_t)key)
#define FMcount(key) (key >> 32)

#define FMencode(count, taxid) (((std::uint64_t)count << 32) | taxid)


namespace emp {


template<std::uint64_t (*score)(std::uint64_t, void *)>
khash_t(64) *feature_count_map(std::vector<std::string> fns, const Spacer &sp, int num_threads=8);
std::uint32_t get_taxid(const char *fn, khash_t(name) *name_hash);

khash_t(c) *make_depth_hash(khash_t(c) *lca_map, khash_t(p) *tax_map);
void lca2depth(khash_t(c) *lca_map, khash_t(p) *tax_map);
template<std::uint64_t (*score)(std::uint64_t, void *)>
int fill_set_seq(kseq_t *ks, const Spacer &sp, khash_t(all) *ret);

void update_lca_map(khash_t(c) *kc, khash_t(all) *set, khash_t(p) *tax, std::uint32_t taxid);
void update_td_map(khash_t(64) *kc, khash_t(all) *set, khash_t(p) *tax, std::uint32_t taxid);
khash_t(64) *make_taxdepth_hash(khash_t(c) *kc, khash_t(p) *tax);

inline void update_feature_counter(khash_t(64) *kc, khash_t(p) *tax, khash_t(all) *set, const std::uint32_t taxid) {
    int khr;
    khint_t k2;
    for(khiter_t ki(kh_begin(set)); ki != kh_end(set); ++ki) {
        if(kh_exist(set, ki)) {
           if((k2 = kh_get(64, kc, kh_key(set, ki))) == kh_end(kc)) {
                k2 = kh_put(64, kc, kh_key(set, ki), &khr);
                kh_val(kc, k2) = FMencode(1, node_depth(tax, taxid));
            } else kh_val(kc, k2) = FMencode(FMcount(kh_val(kc, k2)), lca(tax, taxid, kh_val(kc, k2)));
        }
    }
}


// Return value: whether or not additional sequences were present and added.
template<std::uint64_t (*score)(std::uint64_t, void *)>
int fill_set_seq(kseq_t *ks, const Spacer &sp, khash_t(all) *ret) {
    assert(ret);
    Encoder<score> enc(0, 0, sp, nullptr);
    int khr; // khash return value. Unused, really.
    std::uint64_t kmer;
    if(kseq_read(ks) >= 0) {
        enc.assign(ks);
        while(enc.has_next_kmer())
            if((kmer = enc.next_minimizer()) != BF)
                kh_put(all, ret, kmer, &khr);
        return 1;
    } else return 0;
}

template<std::uint64_t (*score)(std::uint64_t, void *)>
std::size_t fill_set_genome(const char *path, const Spacer &sp, khash_t(all) *ret, std::size_t index, void *data) {
    LOG_ASSERT(ret);
    LOG_INFO("Filling from genome at path %s\n", path);
    gzFile ifp(gzopen(path, "rb"));
    if(!ifp) {
        fprintf(stderr, "Could not open file %s for index %zu. Abort!\n", path, index);
        std::exit(EXIT_FAILURE);
    }
    Encoder<score> enc(0, 0, sp, data);
    kseq_t *ks(kseq_init(ifp));
    int khr; // khash return value. Unused, really.
    std::uint64_t kmer;
    if(sp.w_ > sp.k_) {
        while(kseq_read(ks) >= 0) {
            enc.assign(ks);
            while(likely(enc.has_next_kmer())) {
                if((kmer = enc.next_minimizer()) != BF)
                    kh_put(all, ret, kmer, &khr);
            }
        }
    } else {
        while(kseq_read(ks) >= 0) {
            enc.assign(ks);
            while(likely(enc.has_next_kmer()))
                if((kmer = enc.next_kmer()) != BF)
                    kh_put(all, ret, kmer, &khr);
        }
    }
    kseq_destroy(ks);
    gzclose(ifp);
    LOG_INFO("Set of size %lu filled from genome at path %s\n", kh_size(ret), path);
    return index;
}

template<std::uint64_t (*score)(std::uint64_t, void *)>
khash_t(64) *ftct_map(std::vector<std::string> &fns, khash_t(p) *tax_map,
                      const char *seq2tax_path,
                      const Spacer &sp, int num_threads, std::size_t start_size) {
    return feature_count_map<score>(fns, tax_map, seq2tax_path, sp, num_threads, start_size);
}

void update_minimized_map(khash_t(all) *set, khash_t(64) *full_map, khash_t(c) *ret, int mode);

template<std::uint64_t (*score)(std::uint64_t, void *)>
khash_t(c) *minimized_map(std::vector<std::string> fns,
                          khash_t(64) *full_map,
                          const Spacer &sp, int num_threads, std::size_t start_size=1<<16, int mode=score_scheme::LEX) {
    std::size_t submitted(0), completed(0), todo(fns.size());
    std::vector<khash_t(all) *> counters(todo, nullptr);
    khash_t(c) *ret(kh_init(c));
    kh_resize(c, ret, start_size);
    std::vector<std::future<std::size_t>> futures;
    //for(auto &i: fns) fprintf(stderr, "Filename: %s\n", i.data());

    if(num_threads < 0) num_threads = 16;

    LOG_DEBUG("Number of items to do: %zu\n", todo);

    for(std::size_t i(0); i < todo; ++i) counters[i] = kh_init(all), kh_resize(all, counters[i], start_size);

    // Submit the first set of jobs
    for(int i(0), e(std::min(num_threads, (int)todo)); i < e; ++i) {
        futures.emplace_back(std::async(
          std::launch::async, fill_set_genome<score>, fns.at(i).data(), sp, counters[i], i, (void *)full_map));
        ++submitted;
    }

    // Daemon -- check the status of currently running jobs, submit new ones when available.
    while(submitted < todo) {
        //LOG_DEBUG("Submitted %zu, todo %zu\n", submitted, todo);
        for(auto f(futures.begin()), fend(futures.end()); f != fend; ++f) {
            if(is_ready(*f)) {
                const std::size_t index(f->get());
                futures.erase(f);
                futures.emplace_back(std::async(
                     std::launch::async, fill_set_genome<score>, fns[submitted].data(),
                     sp, counters[submitted], submitted, (void *)full_map));
                LOG_INFO("Submitted for %zu. Updating map for %zu. Total completed/all: %zu/%zu. Current size: %zu\n",
                         submitted, index, completed, todo, kh_size(ret));
                ++submitted, ++completed;
                update_minimized_map(counters[index], full_map, ret, mode);
                kh_destroy(all, counters[index]); // Destroy set once we're done with it.
                break;
            }
        }
    }

    // Join
    for(auto &f: futures) if(f.valid()) {
        const std::size_t index(f.get());
        update_minimized_map(counters[index], full_map, ret, mode);
        kh_destroy(all, counters[index]);
        ++completed;
        LOG_DEBUG("Number left to do: %zu\n", todo - completed);
    }
    LOG_DEBUG("Finished minimized map building! Subbed %zu, completed %zu.\n", submitted, completed);

    // Clean up
    LOG_DEBUG("Cleaned up after LCA map building!\n")
    return ret;
}

template<std::uint64_t (*score)(std::uint64_t, void *)>
khash_t(64) *taxdepth_map(std::vector<std::string> &fns, khash_t(p) *tax_map,
                          const char *seq2tax_path, const Spacer &sp,
                          int num_threads, std::size_t start_size=1<<10) {
    std::size_t submitted(0), completed(0), todo(fns.size());
    khash_t(all) **counters((khash_t(all) **)malloc(sizeof(khash_t(all) *) * todo));
    khash_t(64) *ret(kh_init(64));
    kh_resize(64, ret, start_size);
    khash_t(name) *name_hash(build_name_hash(seq2tax_path));
    std::vector<std::future<std::size_t>> futures;

    for(std::size_t i(0), end(fns.size()); i != end; ++i) counters[i] = kh_init(all);

    // Submit the first set of jobs
    std::set<std::size_t> subbed, used;
    for(int i(0), e(std::min(num_threads, (int)todo)); i < e; ++i) {
        LOG_DEBUG("Launching thread to read from file %s.\n", fns[i].data());
        futures.emplace_back(std::async(
          std::launch::async, fill_set_genome<score>, fns[i].data(), sp, counters[i], i, nullptr));
        //LOG_DEBUG("Submitted for %zu.\n", submitted);
        subbed.insert(submitted);
        ++submitted;
    }

    // Daemon -- check the status of currently running jobs, submit new ones when available.
    while(submitted < todo) {
        LOG_DEBUG("Submitted %zu, todo %zu\n", submitted, todo);
        for(auto &f: futures) {
            if(is_ready(f)) {
                if(submitted == todo) break;
                const std::size_t index(f.get());
                if(used.find(index) != used.end()) continue;
                used.insert(index);
                if(subbed.find(submitted) != subbed.end()) throw "a party!";
                LOG_DEBUG("Launching thread to read from file %s.\n", fns[submitted].data());
                f = std::async(
                  std::launch::async, fill_set_genome<score>, fns[submitted].data(),
                  sp, counters[submitted], submitted, nullptr);
                subbed.insert(submitted);
                LOG_INFO("Submitted for %zu. Updating map for %zu. Total completed/all: %zu/%zu. Total size: %zu.\n",
                         submitted, index, completed, todo, kh_size(ret));
                ++submitted, ++completed;
                const std::uint32_t taxid(get_taxid(fns[index].data(), name_hash));
                LOG_DEBUG("Just fetched taxid from file %s %u.\n", fns[index].data(), taxid);
                update_td_map(ret, counters[index], tax_map, taxid);
                kh_destroy(all, counters[index]); // Destroy set once we're done with it.
            }
        }
    }

    // Join
    for(auto &f: futures) if(f.valid()) {
        const std::size_t index(f.get());
        if(used.find(index) != used.end()) continue;
        used.insert(index);
        update_td_map(ret, counters[index], tax_map, get_taxid(fns[index].data(), name_hash));
        kh_destroy(all, counters[index]);
        ++completed;
        LOG_DEBUG("Number left to do: %zu\n", todo - completed);
    }
    LOG_DEBUG("Finished LCA map building! Subbed %zu, completed %zu, size of futures %zu.\n", submitted, completed, used.size());
#if !NDEBUG
    for(std::size_t i(0); i < todo; ++i) assert(used.find(i) != used.end());
#endif

    // Clean up
    free(counters);
    destroy_name_hash(name_hash);
    LOG_DEBUG("Cleaned up after LCA map building!\n")
    return ret;
}

template<std::uint64_t (*score)(std::uint64_t, void *)>
khash_t(c) *lca_map(std::vector<std::string> fns, khash_t(p) *tax_map,
                    const char *seq2tax_path,
                    const Spacer &sp, int num_threads, std::size_t start_size=1<<10) {
    std::size_t submitted(0), completed(0), todo(fns.size());
    khash_t(all) **counters((khash_t(all) **)malloc(sizeof(khash_t(all) *) * todo));
    khash_t(c) *ret(kh_init(c));
    kh_resize(c, ret, start_size);
    LOG_DEBUG("Building name hash from %s\n", seq2tax_path);
    khash_t(name) *name_hash(build_name_hash(seq2tax_path));
    std::vector<std::future<std::size_t>> futures;

    for(std::size_t i(0), end(fns.size()); i != end; ++i) counters[i] = kh_init(all);

    // Submit the first set of jobs
    std::set<std::size_t> subbed, used;
    for(int i(0), e(std::min(num_threads, (int)todo)); i < e; ++i) {
        LOG_DEBUG("Launching thread to read from file %s.\n", fns[i].data());
        futures.emplace_back(std::async(
          std::launch::async, fill_set_genome<score>, fns[i].data(), sp, counters[i], i, nullptr));
        //LOG_DEBUG("Submitted for %zu.\n", submitted);
        subbed.insert(submitted);
        ++submitted;
    }

    // Daemon -- check the status of currently running jobs, submit new ones when available.
    while(submitted < todo) {
        LOG_DEBUG("Submitted %zu, todo %zu\n", submitted, todo);
        for(auto &f: futures) {
            if(is_ready(f)) {
                if(submitted == todo) break;
                const std::size_t index(f.get());
                if(used.find(index) != used.end()) continue;
                used.insert(index);
                if(subbed.find(submitted) != subbed.end()) throw "a party!";
                LOG_DEBUG("Launching thread to read from file %s.\n", fns[submitted].data());
                f = std::async(
                  std::launch::async, fill_set_genome<score>, fns[submitted].data(),
                  sp, counters[submitted], submitted, nullptr);
                subbed.insert(submitted);
                LOG_INFO("Submitted for %zu. Updating map for %zu. Total completed/all: %zu/%zu. Total size: %zu\n",
                         submitted, index, completed, todo, kh_size(ret));
                ++submitted, ++completed;
                const std::uint32_t taxid(get_taxid(fns[index].data(), name_hash));
                LOG_DEBUG("Just fetched taxid from file %s %u.\n", fns[index].data(), taxid);
                update_lca_map(ret, counters[index], tax_map, taxid);
                kh_destroy(all, counters[index]); // Destroy set once we're done with it.
            }
        }
    }

    // Join
    for(auto &f: futures) if(f.valid()) {
        const std::size_t index(f.get());
        if(used.find(index) != used.end()) continue;
        used.insert(index);
        update_lca_map(ret, counters[index], tax_map, get_taxid(fns[index].data(), name_hash));
        kh_destroy(all, counters[index]);
        ++completed;
        LOG_DEBUG("Number left to do: %zu\n", todo - completed);
    }
    LOG_DEBUG("Finished LCA map building! Subbed %zu, completed %zu, size of futures %zu.\n", submitted, completed, used.size());
#if !NDEBUG
    for(std::size_t i(0); i < todo; ++i) assert(used.find(i) != used.end());
#endif

    // Clean up
    free(counters);
    destroy_name_hash(name_hash);
    LOG_DEBUG("Cleaned up after LCA map building!\n")
    return ret;
}

template <std::uint64_t (*score)(std::uint64_t, void *)>
khash_t(64) *feature_count_map(std::vector<std::string> fns, khash_t(p) *tax_map, const char *seq2tax_path, const Spacer &sp, int num_threads, std::size_t start_size) {
    // Update this to include tax ids in the hash map.
    std::size_t submitted(0), completed(0), todo(fns.size());
    khash_t(all) **counters((khash_t(all) **)malloc(sizeof(khash_t(all) *) * todo));
    khash_t(64) *ret(kh_init(64));
    kh_resize(64, ret, start_size);
    khash_t(name) *name_hash(build_name_hash(seq2tax_path));
    for(std::size_t i(0), end(fns.size()); i != end; ++i) counters[i] = kh_init(all);
    std::vector<std::future<std::size_t>> futures;
    fprintf(stderr, "Will use tax_map (%p) and seq2tax_map (%s) to assign "
                    "feature-minimized values to all kmers.\n", (void *)tax_map, seq2tax_path);

    // Submit the first set of jobs
    std::set<std::size_t> used;
    for(std::size_t i(0); i < (unsigned)num_threads && i < todo; ++i) {
        futures.emplace_back(std::async(
          std::launch::async, fill_set_genome<score>, fns[i].data(), sp, counters[i], i, nullptr));
        LOG_DEBUG("Submitted for %zu.\n", submitted);
        ++submitted;
    }

    // Daemon -- check the status of currently running jobs, submit new ones when available.
    while(submitted < todo) {
        for(auto &f: futures) {
            if(is_ready(f)) {
                const std::size_t index(f.get());
                if(submitted == todo) break;
                if(used.find(index) != used.end()) continue;
                used.insert(index);
                LOG_DEBUG("Submitted for %zu.\n", submitted);
                f = std::async(
                  std::launch::async, fill_set_genome<score>, fns[submitted].data(),
                  sp, counters[submitted], submitted, nullptr);
                ++submitted, ++completed;
                const std::uint32_t taxid(get_taxid(fns[index].data(), name_hash));
                update_feature_counter(ret, tax_map, counters[index], taxid);
                kh_destroy(all, counters[index]); // Destroy set once we're done with it.
            }
        }
    }

    // Join
    for(auto &f: futures) if(f.valid()) {
        const std::size_t index(f.get());
        const std::uint32_t taxid(get_taxid(fns[index].data(), name_hash));
        update_feature_counter(ret, tax_map, counters[index], taxid);
        kh_destroy(all, counters[index]);
        ++completed;
    }

    // Clean up
    free(counters);
    kh_destroy(name, name_hash);
    return ret;
}

} // namespace emp
#endif // #ifdef _FEATURE_MIN__
