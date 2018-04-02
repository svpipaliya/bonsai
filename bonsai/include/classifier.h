#ifndef _DB_H__
#define _DB_H__
#include <atomic>
#include "kspp/ks.h"
#include "encoder.h"
#include "feature_min.h"
#include "klib/kthread.h"
#include "util.h"

namespace bns {
using tax_counter = linear::counter<tax_t, u16>;
enum output_format: int {
    KRAKEN   = 1,
    FASTQ    = 2,
    EMIT_ALL = 4
};


INLINE void append_taxa_run(const tax_t last_taxa,
                            const u32 taxa_run,
                            kstring_t *bks) {
    // U for unclassified (unambiguous but not in database)
    // A for ambiguous: ambiguous nucleotides
    // Actual taxon otherwise.
    switch(last_taxa) {
        case 0:            kputc_('U', bks); break;
        case (tax_t)-1:    kputc_('A', bks); break;
        default:           kputuw_(last_taxa, bks); break;
    }
    kputc_(':', bks); kputuw_(taxa_run, bks); kputc_('\t', bks);
}


inline void append_taxa_runs(tax_t taxon, const std::vector<tax_t> &taxa, kstring_t *bks) {
    if(taxon) {
        tax_t last_taxa(taxa[0]);
        unsigned taxa_run(1);
        for(unsigned i(1), end(taxa.size()); i != end; ++i) {
            if(taxa[i] == last_taxa) ++taxa_run;
            else {
                append_taxa_run(last_taxa, taxa_run, bks);
                last_taxa = taxa[i];
                taxa_run  = 1;
            }
        }
        append_taxa_run(last_taxa, taxa_run, bks); // Add last run.
        bks->s[bks->l - 1] = '\n'; // We add an extra tab. Replace  that with a newline.
        bks->s[bks->l] = '\0';
    } else kputsn("0:0\n", 4, bks);
}

INLINE void append_counts(u32 count, const char character, kstring_t *ks) {
    if(count) {
        kputc_(character, ks);
        kputc_(':', ks);
        kputuw_(count, ks);
        kputc_('\t', ks);
    }
}

inline void append_fastq_classification(const tax_counter &hit_counts,
                                 const std::vector<tax_t> &taxa,
                                 const tax_t taxon, const u32 ambig_count, const u32 missing_count,
                                 bseq1_t *bs, kstring_t *bks, const int verbose, const int is_paired) {
    char *cms, *cme; // comment start, comment end -- used for using comment in both output reads.
    kputs(bs->name, bks);
    kputc_(' ', bks);
    cms = bks->s + bks->l;
    static const char lut[] {'C', 'U'};
    kputc_(lut[taxon == 0], bks);
    kputc_('\t', bks);
    kputuw_(taxon, bks);
    kputc_('\t', bks);
    kputl(bs->l_seq, bks);
    kputc_('\t', bks);
    append_counts(missing_count, 'M', bks);
    append_counts(ambig_count,   'A', bks);
    if(verbose) append_taxa_runs(taxon, taxa, bks);
    else        bks->s[bks->l - 1] = '\n';
    cme = bks->s + bks->l;
    // And now add the rest of the fastq record
    kputsn_(bs->seq, bs->l_seq, bks);
    kputsn_("\n+\n", 3, bks);
    kputsn_(bs->qual, bs->l_seq, bks);
    kputc_('\n', bks);
    if(is_paired) {
        kputs((bs + 1)->name, bks);
        kputc_(' ', bks);
        kputsn_(cms, (int)(cme - cms), bks); // Add comment section in.
        kputc_('\n', bks);
        kputsn_((bs + 1)->seq, (bs + 1)->l_seq, bks);
        kputsn_("\n+\n", 3, bks);
        kputsn_((bs + 1)->qual, (bs + 1)->l_seq, bks);
        kputc_('\n', bks);
    }
    bks->s[bks->l] = 0;
}



inline void append_kraken_classification(const tax_counter &hit_counts,
                                  const std::vector<tax_t> &taxa,
                                  const tax_t taxon, const u32 ambig_count, const u32 missing_count,
                                  bseq1_t *bs, kstring_t *bks) {
    static const char tbl[]{'C', 'U'};
    kputc_(tbl[!taxon], bks);
    kputc_('\t', bks);
    kputs(bs->name, bks);
    kputc_('\t', bks);
    kputuw_(taxon, bks);
    kputc_('\t', bks);
    kputw(bs->l_seq, bks);
    kputc_('\t', bks);
    append_counts(missing_count, 'M', bks);
    append_counts(ambig_count,   'A', bks);
    append_taxa_runs(taxon, taxa, bks);
    bks->s[bks->l] = '\0';
}

template<typename ScoreType>
struct ClassifierGeneric {
    khash_t(c) *db_;
    Spacer sp_;
    Encoder<ScoreType> enc_;
    int nt_;
    int output_flag_;
    std::atomic<u64> classified_[2];
    public:
    void set_emit_all(bool setting) {
        if(setting) output_flag_ |= output_format::EMIT_ALL;
        else        output_flag_ &= (~output_format::EMIT_ALL);
    }
    void set_emit_kraken(bool setting) {
        if(setting) output_flag_ |= output_format::KRAKEN;
        else        output_flag_ &= (~output_format::KRAKEN);
    }
    void set_emit_fastq(bool setting) {
        if(setting) output_flag_ |= output_format::FASTQ;
        else        output_flag_ &= (~output_format::FASTQ);
    }
    INLINE int get_emit_all()    {return output_flag_ & output_format::EMIT_ALL;}
    INLINE int get_emit_kraken() {return output_flag_ & output_format::KRAKEN;}
    INLINE int get_emit_fastq()  {return output_flag_ & output_format::FASTQ;}
    ClassifierGeneric(khash_t(c) *map, spvec_t &spaces, u8 k, std::uint16_t wsz, int num_threads=16,
                      bool emit_all=true, bool emit_fastq=true, bool emit_kraken=false, bool canonicalize=true):
        db_(map),
        sp_(k, wsz, spaces),
        enc_(sp_, canonicalize),
        nt_(num_threads > 0 ? num_threads: 16),
        classified_{0, 0}
    {
        set_emit_all(emit_all);
        set_emit_fastq(emit_fastq);
        set_emit_kraken(emit_kraken);
    }
    ClassifierGeneric(const char *dbpath, spvec_t &spaces, u8 k, std::uint16_t wsz, int num_threads=16,
                      bool emit_all=true, bool emit_fastq=true, bool emit_kraken=false, bool canonicalize=true):
        ClassifierGeneric(khash_load<khash_t(c)>(dbpath), spaces, k, wsz, num_threads, emit_all, emit_fastq, emit_kraken, canonicalize) {}
    u64 n_classified()   const {return classified_[0];}
    u64 n_unclassified() const {return classified_[1];}
};

template<typename ScoreType>
unsigned classify_seq(ClassifierGeneric<ScoreType> &c,
                      Encoder<ScoreType> &enc,
                      const khash_t(p) *taxmap, bseq1_t *bs, const int is_paired, std::vector<tax_t> &taxa) {
    khiter_t ki;
    linear::counter<tax_t, u16> hit_counts;
    u32 ambig_count(0), missing_count(0);
    tax_t taxon(0);
    ks::string bks(bs->sam, bs->l_sam);
    bks.clear();
    taxa.clear();

    auto fn = [&] (u64 kmer) {
        //If the kmer is missing from our database, just say we don't know what it is.
        if((ki = kh_get(c, c.db_, kmer)) == kh_end(c.db_))
            ++missing_count, taxa.push_back(0);
        else
            taxa.push_back(kh_val(c.db_, ki)), hit_counts.add(kh_val(c.db_, ki));
    };
    enc.for_each(fn, bs->seq, bs->l_seq);
    // This simplification loses information about the run of congituous labels. Do these matter?
    auto diff = bs->l_seq - enc.sp_.c_ + 1 - taxa.size();
    taxa.reserve((taxa.size() + diff) << 1);
    ambig_count += diff;
    while(diff--) taxa.push_back(-1);
    if(is_paired) enc.for_each(fn, (bs + 1)->seq, (bs + 1)->l_seq);
    diff = (bs + 1)->l_seq - enc.sp_.c_ + 1;
    ambig_count += diff;
    while(diff--) taxa.push_back(-1); // Consider making this just be a count.

    ++c.classified_[!(taxon = resolve_tree(hit_counts, taxmap))];
    if(taxon || c.get_emit_all()) {
        if(c.get_emit_fastq())
            append_fastq_classification(hit_counts, taxa, taxon, ambig_count, missing_count, bs, kspp2ks(bks), c.get_emit_kraken(), is_paired);
        else if(c.get_emit_kraken())
            append_kraken_classification(hit_counts, taxa, taxon, ambig_count, missing_count, bs, kspp2ks(bks));
    }
    bs->sam          = bks.release();
    return bs->l_sam = bks.size();
}

namespace {
struct kt_data {
    ClassifierGeneric<score::Lex> &c_;
    const khash_t(p) *taxmap;
    bseq1_t *bs_;
    const unsigned per_set_;
    const unsigned total_;
    std::atomic<u64> &retstr_size_;
    const int is_paired_;
};
}

inline void kt_for_helper(void *data_, long index, int tid) {
    kt_data *data((kt_data *)data_);
    size_t retstr_size(0);
    const int inc(!!data->is_paired_ + 1);
    Encoder<score::Lex> enc(data->c_.enc_);
    std::vector<tax_t> taxa;
    for(unsigned i(index * data->per_set_),end(std::min(i + data->per_set_, data->total_));
        i < end;
        i += inc)
            retstr_size += classify_seq(data->c_, enc, data->taxmap, data->bs_ + i, data->is_paired_, taxa);
    data->retstr_size_ += retstr_size;
}



using Classifier = ClassifierGeneric<score::Lex>;

inline void classify_seqs(Classifier &c, const khash_t(p) *taxmap, bseq1_t *bs,
                          kstring_t *cks, const unsigned chunk_size, const unsigned per_set, const int is_paired) {
    assert(per_set && ((per_set & (per_set - 1)) == 0));

    std::atomic<u64> retstr_size(0);
    kt_data data{c, taxmap, bs, per_set, chunk_size, retstr_size, is_paired};
    kt_for(c.nt_, &kt_for_helper, (void *)&data, chunk_size / per_set + 1);
    ks_resize(cks, retstr_size.load());
    const int inc(!!is_paired + 1);
    for(unsigned i(0); i < chunk_size; i += inc) kputsn_(bs[i].sam, bs[i].l_sam, cks);
    cks->s[cks->l] = 0;
}

struct del_data {
    bseq1_t *seqs_;
    unsigned per_set_;
    unsigned total_;
};

inline void process_dataset(Classifier &c, const khash_t(p) *taxmap, const char *fq1, const char *fq2,
                     std::FILE *out, unsigned chunk_size,
                     unsigned per_set) {
    // TODO: consider reusing buffers for processing large numbers of files.
    int nseq(0), max_nseq(0);
    gzFile ifp1(gzopen(fq1, "rb")), ifp2(fq2 ? gzopen(fq2, "rb"): nullptr);
    kseq_t *ks1(kseq_init(ifp1)), *ks2(ifp2 ? kseq_init(ifp2): nullptr);
    del_data dd{nullptr, per_set, chunk_size};
    ks::string cks(256u);
    const int fn = fileno(out);
    const int is_paired(fq2 != 0);
    if((dd.seqs_ = bseq_read(chunk_size, &nseq, (void *)ks1, (void *)ks2)) == nullptr) {
        LOG_WARNING("Could not get any sequences from file, fyi.\n");
        goto fail; // Wheeeeee
    }
    max_nseq = std::max(max_nseq, nseq);
    while((dd.seqs_ = bseq_realloc_read(chunk_size, &nseq, (void *)ks1, (void *)ks2, dd.seqs_))) {
        LOG_INFO("Read %i seqs with chunk size %u\n", nseq, chunk_size);
        max_nseq = std::max(max_nseq, nseq);
        // Classify
        classify_seqs(c, taxmap, dd.seqs_, kspp2ks(cks), nseq, per_set, is_paired);
        // Write out
        cks.write(fn);
        cks.clear();
    }
    // No use parallelizing the frees, there's a global lock on the freeing anyhow.
    for(int i(0); i < max_nseq; bseq_destroy(dd.seqs_ + i++));
    free(dd.seqs_);
    fail:
    // Clean up.
    gzclose(ifp1); gzclose(ifp2);
    kseq_destroy(ks1); kseq_destroy(ks2);
}


} // namespace bns

#endif // #ifndef _DB_H__

