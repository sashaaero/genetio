// Library for I/O of genetic data
// Author: Amy Williams <alw289  cornell edu>
//
// This program is distributed under the terms of the GNU General Public License

#include <string.h>
#include <ctype.h>
#include <cmath>
#include <htslib/hts.h>
#include <htslib/hfile.h>
#include <htslib/vcf.h>
#include <htslib/tbx.h>
#include <htslib/bgzf.h>
#include "personio.h"
#include "personbits.h"
#include "personnorm.h"
#include "marker.h"
#include "util.h"

// Method to read data stored in the various supported types. Detects the file
// type and calls the appropriate methods to parse them.
// <genoFile> is the filename for the genotype data
// <markerFile> is the filename for the file with metadata about the marker
//    (a .snp, .map, or .bim file)
// <indFile> is the filename for the per-individual information
// <onlyChr> if non-NULL, is the name of the specific chromosome that should
//           be analyzed
// <startPos> is the starting position for an analysis of a partial chromosome
// <endPos> is the ending position for an analysis of a partial chromosome
// <XchrName> if non-NULL, gaves the name of the X chromosome
// <noFamilyId> if non-zero, when reading PLINK format data, do not print
//            the family ids in the output file
// <printTrioKids> if non-zero, should print the haplotypes for trio
//                 children
// <numMendelError> if non-NULL, is to be assigned a newly allocated array
//                  with the number of non-Mendelian errors in the data for
//                  each site. Array is expected to have the same number of
//                  indexes as there are markers in the data
// <numMendelCounter> if non-NULL, is the number of trios/duos examined for
//                    Mendelian errors; a denominator for <numMendelError>
template <class P>
void PersonIO<P>::readData(const char *genoFile, const char *markerFile,
			   const char *indFile, const char *onlyChr,
			   int startPos, int endPos, const char *XchrName,
			   int noFamilyId, bool vcfInput,
			   int printTrioKids, FILE *log, int **numMendelError,
			   int **numMendelCounted) {
  if (vcfInput) {
    readVCF(genoFile, onlyChr, startPos, endPos, XchrName, log);
    return;
  }

  FILE *outs[2] = { stdout, log };

  // Not VCF input:
  // open genotype file and determine file type:
  FILE *genoIn = fopen(genoFile, "r");
  if (!genoIn) {
    for (int o = 0; o < 2; o++) {
      FILE *out = outs[o];
      if (out == NULL)
	continue;
      fprintf(out, "\n\nERROR: Couldn't open genotype file %s\n", genoFile);
    }
    perror(genoFile);
    exit(2);
  }

  // defaults to eigenstrat format, but the first byte in the genotype file
  // indicate this (Packed ancestry map begin with the letters 'GENO',
  // and PLINK BED begin with the byte value 108).
  int fileType = 0;

  int c = fgetc(genoIn);
  ungetc(c, genoIn);
  if (c == 'G')
    fileType = 1; // packed ancestry map
  else if (c == 108)
    fileType = 2; // PLINK BED

  ///////////////////////////////////////////////////////////////////////
  // Parse SNP file:

  for (int o = 0; o < 2; o++) {
    FILE *out = outs[o];
    if (out == NULL)
      continue;
    fprintf(out, "Parsing SNP file... ");
  }
  fflush(stdout);

  if (fileType == 0 || fileType == 1) {
    Marker::readSNPFile(markerFile, onlyChr, startPos, endPos);
  }
  else {
    assert(fileType == 2);
    Marker::readBIMFile(markerFile, onlyChr, startPos, endPos);
  }

  for (int o = 0; o < 2; o++) {
    FILE *out = outs[o];
    if (out == NULL)
      continue;
    fprintf(out, "done.\n");

    if (Marker::getNumMarkers() == 0) {
      fprintf(out, "\n");
      fprintf(out, "ERROR: no markers to process.\n");
      exit(1);
    }
  }

  ///////////////////////////////////////////////////////////////////////
  // Parse individual file:

  for (int o = 0; o < 2; o++) {
    FILE *out = outs[o];
    if (out == NULL)
      continue;
    fprintf(out, "Parsing individual file... ");
  }
  fflush(stdout);

  bool mightHaveParents = false;
  FILE *indivIn = fopen(indFile, "r");
  if (!indivIn) {
    for (int o = 0; o < 2; o++) {
      FILE *out = outs[o];
      if (out == NULL)
	continue;
      fprintf(out, "\n\nERROR: Couldn't open individual file %s\n", indFile);
    }
    perror(indFile);
    exit(2);
  }

  if (fileType == 0 || fileType == 1) {
    readIndivs(indivIn);
  }
  else {
    assert(fileType == 2);
    mightHaveParents = readPedOrFamFile(indivIn, noFamilyId,
					/*knowIsFam=*/ true);
  }

  for (int o = 0; o < 2; o++) {
    FILE *out = outs[o];
    if (out == NULL)
      continue;
    fprintf(out, "done.\n");
  }

  // TODO: fix this
  bool analyzingX = strcmp(Marker::getMarker(0)->getChromName(), XchrName) == 0;
  if (analyzingX && P::_numSexUnknown > 0) {
    for (int o = 0; o < 2; o++) {
      FILE *out = outs[o];
      if (out == NULL)
	continue;
      fprintf(out, "ERROR: unspecified sex for %d individuals\n",
	      P::_numSexUnknown);
      fprintf(out, "       analysis of X chromosome requires sex\n");
    }
    exit(3);
  }

  ///////////////////////////////////////////////////////////////////////
  // Parse genotype file:

  for (int o = 0; o < 2; o++) {
    FILE *out = outs[o];
    if (out == NULL)
      continue;
    fprintf(out, "Parsing genotype file... ");
  }
  fflush(stdout);

  if (fileType == 0) {
    parseEigenstratFormat(genoIn);
  }
  else if (fileType == 1) {
    parsePackedAncestryMapFormat(genoIn);
  }
  else {
    assert(fileType == 2);
    parsePlinkBedFormat(genoIn);
  }

  for (int o = 0; o < 2; o++) {
    FILE *out = outs[o];
    if (out == NULL)
      continue;
    fprintf(out, "done.\n");
  }

  fclose(genoIn);

  if (analyzingX) {
    // Set any heterozygous sites on the X to missing in males:
    int length = P::_allIndivs.length();
    for(int p = 0; p < length; p++) {
      P *cur = P::_allIndivs[p];
      if (cur->getSex() == 'M') {
	int numHets, numCalls;
	// TODO: want to call this regardless of whether we're analyzing X alone
	// or if it is read with everything else
	cur->setXHetToMissing(&numHets, &numCalls);
	if (numHets > 0) {
	  for (int o = 0; o < 2; o++) {
	    FILE *out = outs[o];
	    if (out == NULL)
	      continue;
	    fprintf(out, "WARNING: %d/%d chrX heterozygous sites set missing in id %s\n",
		    numHets, numCalls, cur->getId());
	  }
	}
      }
    }
  }

  if (numMendelError != NULL || numMendelCounted != NULL) {
    if (numMendelError == NULL || numMendelCounted == NULL) {
      for (int o = 0; o < 2; o++) {
	FILE *out = outs[o];
	if (out == NULL)
	  continue;
	fprintf(out, "ERROR: must have both numMendelError and numMendelConter non-NULL if one is\n");
      }
      exit(1);
    }

    if (!mightHaveParents) {
      for (int o = 0; o < 2; o++) {
	FILE *out = outs[o];
	if (out == NULL)
	  continue;
	fprintf(out, "ERROR: no family relationships in fam file: can't detect Mendelian errors\n");
      }
      exit(4);
    }

    // Allocate space to store the NME rates, and initialize:
    int numMarkers = Marker::getNumMarkers();
    *numMendelError   = new int[numMarkers];
    *numMendelCounted = new int[numMarkers];
    for (int i = 0; i < numMarkers; i++) {
      (*numMendelError)[i] = (*numMendelCounted)[i] = 0;
    }
  }

  if (mightHaveParents) {
    for (int o = 0; o < 2; o++) {
      FILE *out = outs[o];
      if (out == NULL)
	continue;
//      fprintf(out, "Rereading fam file to identify and infer unambiguous trio/duo phase... ");
      fprintf(out, "Rereading fam file to identify family relationships... ");
    }

    findRelationships(indivIn, log, noFamilyId, *numMendelError,
		      *numMendelCounted);
    for (int o = 0; o < 2; o++) {
      FILE *out = outs[o];
      if (out == NULL)
	continue;
      fprintf(out, "done.\n");
    }
  }

  fclose(indivIn);

  P::cleanUpPostParse(printTrioKids);

  removeIgnoreIndivs();
}

// Method to read data stored in a VCF file.
// <vcfFile> is the filename for the VCF file
// <onlyChr> if non-NULL, is the name of the specific chromosome that should
//           be analyzed
// <startPos> is the starting position for an analysis of a partial chromosome
// <endPos> is the ending position for an analysis of a partial chromosome
// <XchrName> if non-NULL, gaves the name of the X chromosome
template <class P>
void PersonIO<P>::readVCF(const char *vcfFile, const char *onlyChr,
			  int startPos, int endPos, const char *XchrName,
			  FILE *log) {
  FILE *outs[2] = { stdout, log };

  // open with HTSlib:
  hFILE *hfile = hopen(vcfFile, "r");
  if (!hfile) {
    for (int o = 0; o < 2; o++) {
      FILE *out = outs[o];
      if (out == NULL)
	continue;
      fprintf(out, "ERROR: couldn't open %s for reading\n", vcfFile);
    }
    perror(vcfFile);
    exit(2);
  }

  // ensure this is a VCF file:
  htsFormat fmt;
  if (hts_detect_format(hfile, &fmt) < 0) {
    for (int o = 0; o < 2; o++) {
      FILE *out = outs[o];
      if (out == NULL)
	continue;
      fprintf(out, "ERROR: couldn't detect file type of %s\n", vcfFile);
    }
    perror(vcfFile);
    exit(2);
  }

//  if (fmt.category != variant_data) { } // this allows either VCF or BCF
  if (fmt.format != vcf) {
    for (int o = 0; o < 2; o++) {
      FILE *out = outs[o];
      if (out == NULL)
	continue;
      fprintf(out, "ERROR: file %s is not in VCF/BCF format\n", vcfFile);
    }
    exit(3);
  }

  // Now use HTSlib types to open this variant format file; note that
  // VCF and BCF are identical here (see below) for some VCF specific code
  htsFile *vcfIn = hts_hopen(hfile, vcfFile, "r");
  if (!vcfIn) {
    for (int o = 0; o < 2; o++) {
      FILE *out = outs[o];
      if (out == NULL)
	continue;
      fprintf(out, "ERROR: couldn't open %s as a VCF/BCF file\n", vcfFile);
    }
    perror(vcfFile);
    exit(2);
  }

  // read header
  bcf_hdr_t *header = bcf_hdr_read(vcfIn);

  // Note: the tabix loading and tbx_itr_{query,next} code is specific to VCF
  // files. It would be simple to support BCF; see tabix.c query_regions()
  // for an example of analogous code for BCF parsing.
  // Also see htsfile.c view_vcf(); The following is adapted from that and
  // works to parse both BCF and VCF. It does do parsing into the bcf1_t struct
//  htsFile *out = hts_open("-", "w");
//  bcf1_t *rec = bcf_init();
//  for (int i = 0; i < 500; i++) {
//    bcf_read(vcfIn, header, rec);
//    bcf_write(out, header, rec);
//  }
//  bcf_destroy(rec); // Are we calling these everywhere we need to?

  // Load tabix index file
  tbx_t *index = tbx_index_load(vcfFile);
  if (!index) {
    for (int o = 0; o < 2; o++) {
      FILE *out = outs[o];
      if (out == NULL)
	continue;
      fprintf(out, "ERROR: couldn't open %s.tbi for reading\n", vcfFile);
    }
    char *buf = new char[ sizeof(vcfFile) + 5 ];
    sprintf(buf, "%s.tbi", vcfFile);
    perror(buf);
    exit(2);
  }

  // this is the number of contigs: DT is dictionary type; there are a fixed
  // number of indexes to n as defined by the constants BCF_DT_*, and each
  // element of n specifies the number of the given things. CTG is contig
  int numContigs = header->n[BCF_DT_CTG];
  // if <numContigs> is more than 1 and the user hasn't specified a chromosome
  // and we should only be reading one chromosome, error:
  if (numContigs > 1 && onlyChr == NULL && Marker::getReadOnlyOneChrom()) {
    for (int o = 0; o < 2; o++) {
      FILE *out = outs[o];
      if (out == NULL)
	continue;
      fprintf(out, "\n\n");
      fprintf(out, "ERROR: markers present from multiple chromosomes.\n");
      fprintf(out, "Please specify a chromosome number to process with the --chr option\n");
    }
    exit(1);
  }

  if (onlyChr == NULL && numContigs == 1) {
    // have only one chromosome: set onlyChr accordingly so that we also use
    // startPos
    onlyChr = Marker::getMarker(0)->getChromName();
  }
  else if (onlyChr == NULL && startPos) {
    // Ignore starting positions if the chromosome to read from isn't defined
    for (int o = 0; o < 2; o++) {
      FILE *out = outs[o];
      if (out == NULL)
	continue;
      fprintf(out, "WARNING: Ignoring starting position with no chromosome defined\n");
    }
  }

  // If a chromosome/region to read is defined, use the tabix index to tell us
  // where in the file to read from, and then go there:
  int chr_tid; // chromosome/contig tabix id
  if (onlyChr) {
    chr_tid = tbx_name2id(index, onlyChr);
  }
  else {
    // according to hts.h this value is used to iterate over the entire file
    chr_tid = HTS_IDX_START;
  }

  // initialize iterator
  hts_itr_t *itr = tbx_itr_queryi(index, chr_tid, startPos, endPos);
  if (itr == 0) {
    for (int o = 0; o < 2; o++) {
      FILE *out = outs[o];
      if (out == NULL)
	continue;
      if (onlyChr)
	fprintf(out, "ERROR: Could not locate position %s:%d-%d\n", onlyChr,
		startPos, endPos);
	fprintf(out, "ERROR: Could not initialize a Tabix iterator\n");
    }
    exit(2);
  }

  // Now read in the marker indexes:
  Marker::readVCFFile(vcfIn, index, itr, startPos, endPos);

  // done iterating
  tbx_itr_destroy(itr);

  // <header->samples> stores the identifiers for the samples, <header->n[2]>
  // stores the number of samples.
  PersonIO<P>::makePersonsFromIds(header->samples, header->n[2]);

  // no longer need header:
  bcf_hdr_destroy(header);

  // restart reading with the iterator as above:
  itr = tbx_itr_queryi(index, chr_tid, startPos, endPos);
  if (itr == 0) {
    for (int o = 0; o < 2; o++) {
      FILE *out = outs[o];
      if (out == NULL)
	continue;
      if (onlyChr)
	fprintf(out, "ERROR: Could not locate position %s:%d-%d\n", onlyChr,
		startPos, endPos);
      fprintf(out, "ERROR: Could not initialize a Tabix iterator\n");
    }
    exit(2);
  }

  // read the genotypes:
  parseVCFGenotypes(vcfIn, index, itr, vcfFile, outs);

  // done iterating
  tbx_itr_destroy(itr);

  // done completely; close files:
  tbx_destroy(index);
//  hts_close(vcfIn); <-- commented out since it is redundant with next line:
  int ret = hclose(hfile);
  if (ret != 0) {
    for (int o = 0; o < 2; o++) {
      FILE *out = outs[o];
      if (out == NULL)
	continue;
      fprintf(out, "ERROR: Closing %s produced an error, continuing ...\n",
	      vcfFile);
    }
  }
}

// Reads the individual file <filename> and stores the resulting individual
// records in <P::_allIndivs>.
template <class P>
void PersonIO<P>::readIndivs(FILE *in) {
  char id[81], pop[81];
  char sex;

  while (fscanf(in, "%80s %c %80s", id, &sex, pop) == 3) {
    // find pop index for the string <pop>
    int popIndex;
    if (strcmp(pop, "Ignore") == 0)
      popIndex = -1;
    else {
      bool foundPop = false;
      for(popIndex = 0; popIndex < P::_popLabels.length(); popIndex++) {
      	if (strcmp(pop, P::_popLabels[popIndex]) == 0) {
      	  foundPop = true;
      	  break;
      	}
      }
      if (!foundPop) {
      	char *newPopLabel = new char[ strlen(pop) + 1 ];
      	strcpy(newPopLabel, pop);
      	P::_popLabels.append( newPopLabel );
      }
    }
    P *p = new P(id, sex, popIndex);

    P::_allIndivs.append(p);
  }
}

// Parses either the individuals from a PLINK format .fam file or a PLINK
// format .ped file and stores the resulting individual records in
// <P::_allIndivs>.
// Returns true if there are individuals with non-0 values for parents, false
// otherwise.  If true, must call findTrioDuos() after reading the genotype
// data.
template <class P>
bool PersonIO<P>::readPedOrFamFile(FILE *in, bool omitFamilyId,
				   bool knowIsFam) {
  bool hasNonZeroParents = false;
  // initially we don't know what format this is unless <knowIsFam> is true:
  bool isFamFile = knowIsFam;
  bool isPedFile = false;

  char familyid[81], personid[81], parentsids[2][81];
  int sex, pheno;
  char fullid[162];

  // Make population labels corresponding to the phenotypes for printing to
  // the output:
  P::_popLabels.append("Unknown");
  P::_popLabels.append("Control");
  P::_popLabels.append("Case");

  /////////////////////////////////////////////////////////////////////////////
  // First, read in all the individuals and create the Person objects:
  while(fscanf(in, "%s %s %s %s %d %d", familyid, personid, parentsids[0],
	       parentsids[1], &sex, &pheno) == 6) {
    int popIndex = (pheno < 0) ? 0 : pheno;

    assert(popIndex <= 2);

    char sexLetter;
    switch(sex){
      case 1 : sexLetter = 'M'; break;
      case 2 : sexLetter = 'F'; break;
      default : sexLetter = 'U'; break;
    }

    P *thePerson;
    if (omitFamilyId) {
      thePerson = new P(personid, sexLetter, popIndex);
    }
    else {
      sprintf(fullid, "%s:%s", familyid, personid);
      short familyIdLength = strlen(familyid);
      thePerson = new P(fullid, sexLetter, popIndex, familyIdLength);
    }
    P::_allIndivs.append(thePerson);

    hasNonZeroParents |= strcmp(parentsids[0], "0") != 0 ||
						strcmp(parentsids[1], "0") != 0;

    ///////////////////////////////////////////////////////////////////////////
    // Figure out the file format
    if (!isFamFile && !isPedFile) {
      int c;
      // read through next non-white space character or end of line
      while(isspace(c = fgetc(in)) && c != '\n');
      if (c == '\n')
	isFamFile = true;
      else
	isPedFile = true;
      ungetc(c, in);
    }

    ///////////////////////////////////////////////////////////////////////////
    if (isPedFile)
      parsePedGenotypes(in, thePerson);

    // read through end of line
    char c;
    while(isspace(c = fgetc(in)) && c != '\n');
    if (c != '\n') { // should be at end of line; if not, bad formating
      fprintf(stderr, "ERROR: trailing characters on line %d\n",
	      P::_allIndivs.length());
      exit(4);
    }
  }

  char c;
  while (isspace(c = fgetc(in)));

  if (c != EOF) {
    fprintf(stderr, "\n\n");
    fprintf(stderr, "ERROR: line %d not in expected PLINK fam format --\n",
	    P::_allIndivs.length() + 1);
    fprintf(stderr,
	    "          family_id person_id father_id mother_id sex phenotype\n");
    fprintf(stderr, "       ids,sex are strings (sex=1 => male, 2 => female), phenotype is an integer\n");
    exit(5);
  }

  return hasNonZeroParents;
}

// Creates records in <P::_allIndivs> for the <numIds> identifiers in <ids>.
// Sets the population and sex labels to unknown
template <class P>
void PersonIO<P>::makePersonsFromIds(char **ids, uint32_t numIds) {
  // No population or sex labels, so:
  P::_popLabels.append("Unknown");
  char sexLetter = 'U';
  for(uint32_t i = 0; i < numIds; i++) {
    P *thePerson = new P(ids[i], sexLetter, /*popIndex=*/ 0);
    P::_allIndivs.append(thePerson);
  }
}

// Reads the genotypes from a PLINK format .ped file (note: this function
// expects that the six fields at the beginning of the line have already
// been parsed; readPedOrFamFile() does this)
template <class P>
void PersonIO<P>::parsePedGenotypes(FILE *in, P *thePerson) {
  // Which haplotype chunk are we on?  (A chunk is BITS_PER_CHUNK bits)
  int curHapChunk = 0;
  // Which bit/locus within the chunk are we on?
  int curChunkIdx = 0;

  // Which marker number does the next genotype correspond to?
  int curMarkerIdx = 0;
  // What marker index on the chromosome does the next genotype correspond to?
  int chromMarkerIdx = 0;
  // Which chromosome index are we currently on?
  int chromIdx = Marker::getMarker(0)->getChromIdx();

  // Note: because SNPs are listed by individual, rather than all data for a
  // given marker being read at once, it is not simple to calculate allele
  // counts for peds, therefore we don't do these counts.
//  // For computing allele frequencies for the current marker:
//  int alleleCount;    // init'd below
//  int totalGenotypes;

  int numMarkersToRead = Marker::getNumMarkers();

  const dynarray<int> &omitMarkers = Marker::getMarkersToOmit();
  int omitIdx = 0;
  int nextToOmitIdx =(omitMarkers.length()>omitIdx) ? omitMarkers[omitIdx] : -1;

  for( ; curMarkerIdx < numMarkersToRead; curMarkerIdx++) {
    char allele[2];

    for(int a = 0; a < 2; a++) {
      while ((allele[a] = fgetc(in))&& (allele[a] == ' ' || allele[a] == '\t'));
      if (!isalnum(allele[a])) {
	fprintf(stderr,"ERROR reading id %s: allele '%c' is non-alphanumeric\n",
		thePerson->getId(), allele[a]);
	exit(2);
      }
    }

    if (curMarkerIdx == nextToOmitIdx) {
      // skip this marker
      omitIdx++;
      nextToOmitIdx =(omitMarkers.length()>omitIdx) ? omitMarkers[omitIdx] : -1;
      // must decrement curMarkerIdx since this index *is* used in the markers
      // that are stored
      curMarkerIdx--;
      continue;
    }

//    alleleCount = 0;
//    totalGenotypes = 0;

    if (Marker::getLastMarkerNum(chromIdx) == curMarkerIdx - 1) {
      // shouldn't be last chrom
      assert(chromIdx < Marker::getNumChroms() - 1);

      // Now on next chromosome; update chunk indices
      if (curChunkIdx != 0) { // markers from prev chrom on current chunk?
	curHapChunk++; // markers for current chrom are on next chunk number
	curChunkIdx = 0;
      }

      chromMarkerIdx = 0; // back to first marker on the new chromosome

      chromIdx = Marker::getMarker(curMarkerIdx)->getChromIdx();
    }

    // TODO: the following is broken and needs to be fixed if we want
    // support for ped files (issue is with storing multiple alleles
//    int geno[2];
//    Marker *curMarker = Marker::getMarkerNonConst(curMarkerIdx);
//    for(int a = 0; a < 2; a++) {
//      if (allele[a] == '0')
//	geno[a] = -1; // missing
//
//      bool found = false;
//      for(int i = 0; i < curMarker->getNumAlleles(); i++) {
//	assert(false);
//	if (allele[a] == curMarker->getAllele(i)) {
//	  geno[a] = i;
//	  found = true;
//	  break;
//	}
//      }
//
//      if (!found) {
//	int i = curMarker->getNumAlleles();
//	curMarker->addAllele(allele[a]);
//	geno[a] = i;
//      }
//    }
//
//    thePerson->setGenotype(curHapChunk, curChunkIdx, chromIdx, chromMarkerIdx,
//			   geno);

//    if (geno[0] >= 0) {
//      // Note: this is wrong for multiallelic variants
//      alleleCount += geno[0] + geno[1];
//      totalGenotypes++;
//    }
//    // store away allele frequency:
//    Marker::getMarkerNonConst(curMarkerIdx)->setAlleleFreq(alleleCount,
//							   totalGenotypes);

    // increment marker num:
    curChunkIdx++;
    chromMarkerIdx++;
    if (curChunkIdx == BITS_PER_CHUNK) {
      curHapChunk++;
      curChunkIdx = 0;
    }
  }

  // Should have gotten through all markers:
  assert( curMarkerIdx == Marker::getNumMarkers() );
}

// Rereads the PLINK format .fam file to identify and set parents as
// appropriate to connect children with their parents.
// If non-NULL, <numMendelError> is set to be the count of the number of
// Mendelian errors, and <numMendelCounted> is the number of relationships with
// non-missing data that were examined for Mendelian errors. These values are
// assumed to point to arrays with size of the number of markers in the data
// with values initialized to 0.
template <class P>
void PersonIO<P>::findRelationships(FILE *in, FILE *log, bool omitFamilyId,
				    int *numMendelError,
				    int *numMendelCounted) {
  char familyid[81], parentsids[2][81];

  bool warningPrinted = false; // have we printed a warning yet?

  /////////////////////////////////////////////////////////////////////////////
  // Now go through the file again and setup the trio and duo relationships
  rewind(in);
  int curP = 0;
  while(fscanf(in, "%s %*s %s %s %*s %*d", familyid, parentsids[0],
	       parentsids[1]) ==3) {
    assert(curP < P::_allIndivs.length());
    P *thePerson = P::_allIndivs[curP];
    curP++;

    if (strcmp(parentsids[0], "0") == 0 && strcmp(parentsids[1], "0") == 0)
      // no family relationships
      continue;

    //////////////////////////////////////////////////////////////////////////
    // Have at least one parent defined...

    // do we have genotype data for the non-zero parents?  how many parents?
    int numParents = 0;
    bool missingParents = false;
    char fullid[162];
    P *parents[2] = { NULL, NULL };
    for(int p = 0; p < 2; p++) {
      if (strcmp(parentsids[p], "0") == 0)
	continue;

      numParents++;

      char *curParId;
      if (omitFamilyId) {
	curParId = parentsids[p];
      }
      else {
	sprintf(fullid, "%s:%s", familyid, parentsids[p]);
	curParId = fullid;
      }
      parents[p] = P::lookupId(curParId);

      if (parents[p] == NULL) {
	if (!warningPrinted) {
	  printf("\n");
	  if (log != NULL) fprintf(log, "\n");
	  warningPrinted = true;
	}
	printf("WARNING: parent id %s of person %s does not exist\n",
	       curParId, thePerson->getId());
	if (log != NULL)
	  fprintf(log, "WARNING: parent id %s of person %s does not exist\n",
		  curParId, thePerson->getId());
	missingParents = true;
	numParents--;
      }
    }

    if (numParents == 0) {
      assert(missingParents); // must be true
      printf("  no family relationships included for child %s: treating as unrelated\n",
	     thePerson->getId());
      if (log != NULL)
	fprintf(log, "  no family relationships included for child %s: treating as unrelated\n",
		thePerson->getId());
      continue;
    }
    else if (missingParents) {
      assert(numParents == 1); // must be true
      printf("  only one parent for for child %s: treating as duo\n",
	     thePerson->getId());
      if (log != NULL)
	fprintf(log, "  only one parent for child %s: treating as duo\n",
		thePerson->getId());
    }

    assert(numParents > 0);

    for(int p = 0; p < 2; p++) {
      if (parents[p] == NULL)
	continue;

      if (p == 0 && parents[p] != NULL && parents[p]->getSex() == 'F') {
	if (!warningPrinted) {
	  printf("\n");
	  if (log != NULL) fprintf(log, "\n");
	  warningPrinted = true;
	}
	printf("WARNING: father id %s is listed as female elsewhere\n",
	       parents[p]->getId());
	if (log != NULL)
	  fprintf(log, "WARNING: father id %s is listed as female elsewhere\n",
		  parents[p]->getId());
      }
      if (p == 1 && parents[p] != NULL && parents[p]->getSex() == 'M') {
	if (!warningPrinted) {
	  printf("\n");
	  if (log != NULL) fprintf(log, "\n");
	  warningPrinted = true;
	}
	printf("WARNING: mother id %s is listed as male elsewhere\n",
	       parents[p]->getId());
	if (log != NULL)
	  fprintf(log, "WARNING: mother id %s is listed as male elsewhere\n",
		  parents[p]->getId());
      }
    }

    thePerson->setParents(familyid, parents, numParents, warningPrinted,
			  log, numMendelError, numMendelCounted);
  }
}

// Removes from <P::_allIndivs> any individuals with the _ignore field set to
// true
template <class P>
void PersonIO<P>::removeIgnoreIndivs() {
  // Remove individuals marked as "Ignore", preserving the same order of indivs
  int length = P::_allIndivs.length();
  // how many indivs forward should we look to find the current indiv?
  int shiftIdx = 0;
  for (int p = 0; p < length; p++) {
    if (shiftIdx > 0)
      P::_allIndivs[p] = P::_allIndivs[ p + shiftIdx ];

    P *cur = P::_allIndivs[p];
    if (cur->isIgnore()) {
      delete cur;
      shiftIdx++; // should look one more indiv forward to find the indiv at <p>
      // examine this index again as it now references a different indiv:
      p--;
      length--;
    }
  }

  // shorten <P::_allIndivs> by the number of removed indivs:
  if (shiftIdx > 0)
    P::_allIndivs.removeLast(shiftIdx);
}

// Parses a genotype file in Packed AncestryMap format
template <class P>
void PersonIO<P>::parsePackedAncestryMapFormat(FILE *in) {
  int numIndivs = P::_allIndivs.length();
  // file specified num indivs, file specified num markers, hash codes
  int fNumIndivs, fNumMarkers, ihash, shash;

  int bytesPerSNP = std::ceil( ((float) numIndivs * 2) / (8 * sizeof(char)) );
  int recordLen = max(bytesPerSNP, 48);
  char *buf = new char[recordLen];

  int ret = fread(buf, recordLen, sizeof(char), in);
  assert(ret != 0);

  sscanf(buf, "GENO %d %d %x %x", &fNumIndivs, &fNumMarkers, &ihash, &shash);
  if (fNumIndivs != numIndivs) {
    fprintf(stderr, "\nERROR: Number of individuals do not match in indiv and genotype files.\n");
    exit(1);
  }
  if (fNumMarkers != Marker::getNumMarkersInFile()) {
    fprintf(stderr, "\nERROR: Number of markers do not match in snp and genotype files.\n");
    exit(1);
  }

  // Note: not going to check the hash values <ihash> and <shash>. This means
  // people can make changes to the SNP and individual ids without a warning.
  // Potentially should change this at some point, but need to get Nick's
  // hash function to do so.

  parsePackedGenotypes(in, recordLen, buf, numIndivs, /*type=*/ 1);

  delete [] buf;
}

// Common code used to parse the packed genotypes in both packed AncestryMap
// and PLINK BED format files.  <type> gives the file type (since the binary
// encoding and order of samples is different), 1 for AncestryMap, 2 for BED.
template <class P>
void PersonIO<P>::parsePackedGenotypes(FILE *in, int recordLen, char *buf,
				       int numIndivs, int type) {
  assert(type == 1 || type == 2); // only two different possible file types

  int ret;

  // read in but don't store genotypes for markers that should be skipped
  // because we're only analyzing one chromosome:
  for(int i = 0; i < Marker::getFirstStoredMarkerFileIdx(); i++) {
    ret = fread(buf, recordLen, sizeof(char), in);
    if (ret == 0) {
      fprintf(stderr, "\nERROR reading from geno file\n");
      exit(1);
    }
  }


  // Which haplotype chunk are we on?  (A chunk is BITS_PER_CHUNK bits)
  int curHapChunk = 0;
  // Which bit/locus within the chunk are we on?
  int curChunkIdx = 0;

  // Which marker number does the data correspond to?
  int curMarkerIdx = 0;
  // What marker index on the chromosome does the next genotype correspond to?
  int chromMarkerIdx = 0;
  // Which chromosome index are we currently on?
  int chromIdx = Marker::getMarker(0)->getChromIdx();

  // For computing allele frequencies for the current marker:
  int alleleCount = 0;
  int totalGenotypes = 0;

  int numMarkersToRead = Marker::getNumMarkers();

  const dynarray<int> &omitMarkers = Marker::getMarkersToOmit();
  int omitIdx = 0;
  int nextToOmitIdx =(omitMarkers.length()>omitIdx) ? omitMarkers[omitIdx] : -1;

  for( ; curMarkerIdx < numMarkersToRead; curMarkerIdx++) {
    ret = fread(buf, recordLen, sizeof(char), in);
    if (ret == 0) {
      fprintf(stderr, "\nERROR reading from geno file\n");
      exit(1);
    }

    if (curMarkerIdx == nextToOmitIdx) {
      // skip this marker
      omitIdx++;
      nextToOmitIdx =(omitMarkers.length()>omitIdx) ? omitMarkers[omitIdx] : -1;
      // must decrement curMarkerIdx since this index *is* used in the markers
      // that are stored
      curMarkerIdx--;
      continue;
    }

    if (Marker::getLastMarkerNum(chromIdx) == curMarkerIdx - 1) {
      // shouldn't be last chrom
      assert(chromIdx < Marker::getNumChroms() - 1);

      // Now on next chromosome; update chunk indices
      if (curChunkIdx != 0) { // markers from prev chrom on current chunk?
	curHapChunk++; // markers for current chrom are on next chunk number
	curChunkIdx = 0;
      }

      chromMarkerIdx = 0; // back to first marker on the new chromosome

      chromIdx = Marker::getMarker(curMarkerIdx)->getChromIdx();
    }

    int bufCharIdx = 0;
    int charIdx = (type == 1) ? sizeof(char) * 8 - 2 : 0;
    for(int curPersonIdx = 0; curPersonIdx < numIndivs; curPersonIdx++) {
      if (charIdx < 0 || charIdx == sizeof(char) * 8) {
	bufCharIdx++;
	charIdx = (type == 1) ? sizeof(char) * 8 - 2 : 0;
      }
      int genoBits = (buf[ bufCharIdx ] >> charIdx) & 3;
      int geno[2];


      if (type == 1) {
	switch (genoBits) {
	  case 0:
	    geno[0] = geno[1] = 0;
	    break;
	  case 1:
	    geno[0] = 0;
	    geno[1] = 1;
	    break;
	  case 2:
	    geno[0] = geno[1] = 1;
	    break;
	  case 3: // missing data
	    geno[0] = geno[1] = -1;
	    break;
	}
      }
      else {
	assert(type == 2);
	switch (genoBits) {
	  case 0: // (binary 00) homozygous for first allele, so 2 copies of it
	    //genoBits = 2;
	    geno[0] = geno[1] = 1;
	    break;
	  case 1: // missing
	    //genoBits = 3;
	    geno[0] = geno[1] = -1;
	    break;
	  case 2: // heterozygote
	    //genotype = 1;
	    geno[0] = 0;
	    geno[1] = 1;
	    break;
	  case 3: // 3 (binary 11) is homozygous for second allele
	    //genotype = 0; // 0 copies of first allele
	    geno[0] = geno[1] = 0;
	    break;
	}
      }

      P::_allIndivs[curPersonIdx]->setGenotype(curHapChunk, curChunkIdx,
					       chromIdx, chromMarkerIdx, geno);

      if (geno[0] >= 0) {
      	alleleCount += geno[0] + geno[1];
      	totalGenotypes++;
      }

      if (type == 1)
	      charIdx -= 2;
      else
	      charIdx += 2;
    }

    Marker::getMarkerNonConst(curMarkerIdx)->setAlleleFreq(alleleCount,
							   totalGenotypes);
    alleleCount = 0;
    totalGenotypes = 0;

    // increment marker num:
    curChunkIdx++;
    chromMarkerIdx++;
    if (curChunkIdx == BITS_PER_CHUNK) {
      curHapChunk++;
      curChunkIdx = 0;
    }
  }
}

// Parses a genotype file in Eigenstrat format
template <class P>
void PersonIO<P>::parseEigenstratFormat(FILE *in) {
  int c, ret;

  int numSamples = P::_allIndivs.length();
  char *buf = new char[numSamples+1];

  // read in but don't store genotypes for markers that should be skipped
  // because we're only analyzing one chromosome:
  for(int i = 0; i < Marker::getFirstStoredMarkerFileIdx(); i++) {
    // read in one line which will consist of <numSamples> genotypes plus a
    // '\n' character:
    ret = fread(buf, numSamples+1, sizeof(char), in);
    assert(buf[numSamples] == '\n'); // should have endline here
    if (ret == 0) {
      fprintf(stderr, "\nERROR reading from geno file\n");
      exit(1);
    }
  }

  // Which haplotype chunk are we on?  (A chunk is BITS_PER_CHUNK bits)
  int curHapChunk = 0;
  // Which bit/locus within the chunk are we on?
  int curChunkIdx = 0;

  // Which marker number does the data correspond to?  The genotype file has
  // one marker per line, so this value gets incremented every time we encounter
  // a newline
  int curMarkerIdx = 0;
  // What marker index on the chromosome does the next genotype correspond to?
  int chromMarkerIdx = 0;
  // Which chromosome index are we currently on?
  int chromIdx = Marker::getMarker(0)->getChromIdx();

  // For computing allele frequencies for the current marker:
  int alleleCount;    // init'd below
  int totalGenotypes;

  int numMarkersToRead = Marker::getNumMarkers();

  const dynarray<int> &omitMarkers = Marker::getMarkersToOmit();
  int omitIdx = 0;
  int nextToOmitIdx =(omitMarkers.length()>omitIdx) ? omitMarkers[omitIdx] : -1;

  for( ; curMarkerIdx < numMarkersToRead; curMarkerIdx++) {
    ret = fread(buf, numSamples+1, sizeof(char), in);
    assert(buf[numSamples] == '\n'); // should have endline here
    if (ret == 0) {
      fprintf(stderr, "\nERROR reading from geno file\n");
      exit(1);
    }

    if (curMarkerIdx == nextToOmitIdx) {
      // skip this marker
      omitIdx++;
      nextToOmitIdx =(omitMarkers.length()>omitIdx) ? omitMarkers[omitIdx] : -1;
      // must decrement curMarkerIdx since this index *is* used in the markers
      // that are stored
      curMarkerIdx--;
      continue;
    }

    alleleCount = 0;
    totalGenotypes = 0;

    if (Marker::getLastMarkerNum(chromIdx) == curMarkerIdx - 1) {
      // shouldn't be last chrom
      assert(chromIdx < Marker::getNumChroms() - 1);

      // Now on next chromosome; update chunk indices
      if (curChunkIdx != 0) { // markers from prev chrom on current chunk?
	curHapChunk++; // markers for current chrom are on next chunk number
	curChunkIdx = 0;
      }

      chromMarkerIdx = 0; // back to first marker on the new chromosome

      chromIdx = Marker::getMarker(curMarkerIdx)->getChromIdx();
    }

    for(int curPersonIdx = 0; curPersonIdx < numSamples; curPersonIdx++) {
      c = buf[curPersonIdx];

      // the genotype
      if (c != '0' && c != '1' && c != '2' && c != '9') {
      	fprintf(stderr, "\nERROR: bad character in genotype file: %c\n", c);
      	exit(1);
      }
      
      int geno[2];
      switch (c) {
      	case '0':
      	  geno[0] = geno[1] = 0;
      	  break;
      	case '1':
      	  geno[0] = 0;
      	  geno[1] = 1;
      	  break;
      	case '2':
      	  geno[0] = geno[1] = 1;
      	  break;
      	case '9': // missing data
      	  geno[0] = geno[1] = -1;
      	  break;
      }

      P::_allIndivs[curPersonIdx]->setGenotype(curHapChunk, curChunkIdx,
					       chromIdx, chromMarkerIdx, geno);

      if (geno[0] >= 0) {
	alleleCount += geno[0] + geno[1];
	totalGenotypes++;
      }

    }

    // store away allele frequency:
    Marker::getMarkerNonConst(curMarkerIdx)->setAlleleFreq(alleleCount,
							   totalGenotypes);
    // increment marker num:
    curChunkIdx++;
    chromMarkerIdx++;
    if (curChunkIdx == BITS_PER_CHUNK) {
      curHapChunk++;
      curChunkIdx = 0;
    }
  }

  // Should have gotten through all markers:
  assert( curMarkerIdx == Marker::getNumMarkers() );

  delete [] buf;
}

// Parses a genotype file in PLINK .bed format
template <class P>
void PersonIO<P>::parsePlinkBedFormat(FILE *in) {
  if (fgetc(in) != 108 || fgetc(in) != 27) { // check for PLINK BED magic header
    fprintf(stderr, "\nERROR: reading PLINK BED: magic header missing is this a PLINK BED file?\n");
    exit(2);
  }

  if (fgetc(in) != 1) {
    fprintf(stderr, "\nERROR: PLINK BED file in individual-major mode\n");
    fprintf(stderr, "File type not supported; use PLINK to convert to SNP-major mode\n");
    exit(2);
  }

  int numIndivs = P::_allIndivs.length();

  assert(sizeof(char) == 1); // I think this will always hold...

  int bytesPerSNP = std::ceil( ((float) numIndivs * 2) / (8 * sizeof(char)) );
  int recordLen = bytesPerSNP;
  char *buf = new char[recordLen];

  parsePackedGenotypes(in, recordLen, buf, numIndivs, /*type=*/ 2);

  delete [] buf;
}

// Parses genotypes only (not marker information, which is done in the Marker
// class) from a VCF format input stream in <vcfIn>
template <class P>
void PersonIO<P>::parseVCFGenotypes(htsFile *vcfIn, tbx_t *index,
				    hts_itr_t *itr, const char *vcfFile,
				    FILE *outs[2]) {
  int numIndivs = P::_allIndivs.length();

  // Which haplotype chunk are we on?  (A chunk is BITS_PER_CHUNK bits)
  int curHapChunk = 0;
  // Which bit/locus within the chunk are we on?
  int curChunkIdx = 0;

  // Which marker number does the data correspond to?
  int curMarkerIdx = 0;
  // What marker index on the chromosome does the next genotype correspond to?
  int chromMarkerIdx = 0;
  // Which chromosome index are we currently on?
  int chromIdx = Marker::getMarker(0)->getChromIdx();

  // For computing allele frequencies for the current marker:
  int alleleCount;      // init'd below
  int totalGenotypes;
  bool nonStandardGeno; // any genotypes besides 0 and 1? Can't calculate AF

  int numMarkersToRead = Marker::getNumMarkers();

  // TODO: should we delete this dynarray in Marker?
  const dynarray<int> &omitMarkers = Marker::getMarkersToOmit();
  int omitIdx = 0;
  int nextToOmitIdx =(omitMarkers.length()>omitIdx) ? omitMarkers[omitIdx] : -1;

  // Start parsing the VCF using HTSlib; gives one line at a time:
  // Go through all the lines in the query region
  for ( ; tbx_itr_next(vcfIn, index, itr, &vcfIn->line) >= 0; curMarkerIdx++) {
    assert(curMarkerIdx < numMarkersToRead);
    // string for the current line is in vcfIn->line.s

    if (curMarkerIdx == nextToOmitIdx) {
      // skip this marker
      omitIdx++;
      nextToOmitIdx =(omitMarkers.length()>omitIdx) ? omitMarkers[omitIdx] : -1;
      // must decrement curMarkerIdx since this index *is* used in the markers
      // that are stored
      curMarkerIdx--;
      continue;
    }

    alleleCount = 0;
    totalGenotypes = 0;
    nonStandardGeno = false;

    if (Marker::getLastMarkerNum(chromIdx) == curMarkerIdx - 1) {
      // shouldn't be last chrom
      assert(chromIdx < Marker::getNumChroms() - 1);

      // Now on next chromosome; update chunk indices
      if (curChunkIdx != 0) { // markers from prev chrom on current chunk?
	curHapChunk++; // markers for current chrom are on next chunk number
	curChunkIdx = 0;
      }

      chromMarkerIdx = 0; // back to first marker on the new chromosome

      chromIdx = Marker::getMarker(curMarkerIdx)->getChromIdx();
    }

    // ignore tokens 0..7 -- want the FORMAT value (token index 8)
    unsigned int c = 0; // current index into vcfIn->line.s
    for(int token = 0; token <= 7; token++) {
      for( ; vcfIn->line.s[c] != '\t'; c++); // skip until next delimiter: '\t'
      // vcfIn->line.s[c] == '\t' -- go to next c for next token
      c++;
    }

    // according to the spec, the GT data type, if present, must be the first
    // field; double check this:
    if (!(vcfIn->line.s[c] == 'G' && vcfIn->line.s[c+1] == 'T' &&
	  (vcfIn->line.s[c+2] == ':' || vcfIn->line.s[c+2] == '\t'))) {
      for (int o = 0; o < 2; o++) {
	FILE *out = outs[o];
	if (out == NULL)
	  continue;
	fprintf(out, "ERROR: GT data type expected as first format in %s\n",
		vcfFile);
      }
      exit(2);
    }
    c+=2; // skip past "GT"

    // read past the FORMAT string:
    for( ; vcfIn->line.s[c] != '\t'; c++);
    c++; // now skip past the '\t' to the first genotype

    // now read in genotypes for each sample:
    for(int curPersonIdx = 0; curPersonIdx < numIndivs; curPersonIdx++) {
      int geno[2];

      // first genotype
      char tmp = vcfIn->line.s[c];
      c++;
      if (tmp == '.') {
	geno[0] = -1; // missing data
      }
      else {
	if (!isdigit(tmp)) {
	  for (int o = 0; o < 2; o++) {
	    FILE *out = outs[o];
	    if (out == NULL)
	      continue;
	    fprintf(out, "ERROR: first genotype value non-numeric?\n");
	  }
	  exit(2);
	}
	geno[0] = tmp - '0'; // as integer
      }

      // check the character between two genotypes
      char delim = vcfIn->line.s[c];
      c++;
      if (delim != '/' && delim != '|' && delim != ':' && delim != '\t') {
	for (int o = 0; o < 2; o++) {
	  FILE *out = outs[o];
	  if (out == NULL)
	    continue;
	  fprintf(out, "ERROR: expecting genotype of the form x/x or x|x\n");
	}
	exit(2);
      }

      if (delim == ':' || delim == '\t') {
	// only one genotype: is either X chromosome in males, Y, or MT
	// will set to homozygous:
	geno[1] = geno[0];
      }
      else {
	tmp = vcfIn->line.s[c];
	c++;
	if (tmp == '.') {
	  geno[1] = -1; // missing data
	  if (geno[0] != -1) {
	    // TODO: make function for this
	    for (int o = 0; o < 2; o++) {
	      FILE *out = outs[o];
	      if (out == NULL)
		continue;
	      fprintf(out, "ERROR: second genotype missing but first not?\n");
	    }
	    exit(2);
	  }
	}
	else {
	  if (!isdigit(tmp)) {
	    for (int o = 0; o < 2; o++) {
	      FILE *out = outs[o];
	      if (out == NULL)
		continue;
	      fprintf(out, "ERROR: first genotype value non-numeric?\n");
	    }
	    exit(2);
	  }
	  geno[1] = tmp - '0'; // as integer
	}

	delim = (c < vcfIn->line.l) ? vcfIn->line.s[c] : '\t';
	//c++; <-- commented out so we don't skip past '\t' expected below
	if (delim != ':' && delim != '\t') {
	  for (int o = 0; o < 2; o++) {
	    FILE *out = outs[o];
	    if (out == NULL)
	      continue;
	    fprintf(out, "ERROR: expecting genotype of the form x/x or x|x\n");
	    fprintf(out,"       genotype with more than two alleles present\n");
	  }
	  exit(2);
	}
      }

      P::_allIndivs[curPersonIdx]->setGenotype(curHapChunk, curChunkIdx,
					       chromIdx, chromMarkerIdx, geno);

      if (geno[0] > 1 || geno[1] > 1) {
	nonStandardGeno = true;
      }
      else if (geno[0] >= 0) {
	alleleCount += geno[0] + geno[1];
	totalGenotypes++;
      }

      // read to the end of this entry for <curPersonIdx>:
      // note: line->l stores length
      for( ; c < vcfIn->line.l && vcfIn->line.s[c] != '\t'; c++);
      c++; // skip past the '\t'
      if (c >= vcfIn->line.l && curPersonIdx+1 < numIndivs) {
	for (int o = 0; o < 2; o++) {
	  FILE *out = outs[o];
	  if (out == NULL)
	    continue;
	  fprintf(out, "ERROR: read genotypes for %d samples of %d expected\n",
		  curPersonIdx+1, numIndivs);
	}
	exit(2);
      }
    }

    if (c+1 < vcfIn->line.l) {
      for (int o = 0; o < 2; o++) {
	FILE *out = outs[o];
	if (out == NULL)
	  continue;
	fprintf(out, "ERROR: more data than the %d expected samples\n",
		numIndivs);
      }
      exit(2);
    }

    Marker::getMarkerNonConst(curMarkerIdx)->setAlleleFreq(alleleCount,
							   totalGenotypes,
							   nonStandardGeno);
    // increment marker num:
    curChunkIdx++;
    chromMarkerIdx++;
    if (curChunkIdx == BITS_PER_CHUNK) {
      curHapChunk++;
      curChunkIdx = 0;
    }
  }
}

// Prints an eigenstrat-format .geno file to <out>
template <class P>
void PersonIO<P>::printEigenstratGeno(FILE *out) {
  int numMarkers = Marker::getNumMarkers();
  int numIndivs = P::_allIndivs.length();

  // Which haplotype chunk are we on?  (A chunk is BITS_PER_CHUNK bits)
  int curHapChunk = 0;
  // Which bit/locus within the chunk are we on?
  int curChunkIdx = 0;

  // What marker index on the chromosome does the next genotype correspond to?
  int chromMarkerIdx = 0;
  // Which chromosome index are we currently on?
  int chromIdx = Marker::getMarker(0)->getChromIdx();

  for(int m = 0; m < numMarkers; m++) {

    if (Marker::getLastMarkerNum(chromIdx) == m - 1) {
      // shouldn't be last chrom
      assert(chromIdx < Marker::getNumChroms() - 1);

      // Now on next chromosome; update chunk indices
      if (curChunkIdx != 0) { // markers from prev chrom on current chunk?
	curHapChunk++; // markers for current chrom are on next chunk number
	curChunkIdx = 0;
      }

      chromMarkerIdx = 0; // back to first marker on the new chromosome

      chromIdx = Marker::getMarker(m)->getChromIdx();
    }

    for(int i = 0; i < numIndivs; i++) {
      int genotype = P::_allIndivs[i]->getGenotype(curHapChunk, curChunkIdx,
						   chromIdx, chromMarkerIdx);
      fprintf(out, "%d", genotype);
    }
    fprintf(out, "\n");

    curChunkIdx++;
    chromMarkerIdx++;
    if (curChunkIdx == BITS_PER_CHUNK) {
      curHapChunk++;
      curChunkIdx = 0;
    }
  }
}

// Print an eigenstrat-formated .phgeno file with all phased samples to <out>
template <class P>
void PersonIO<P>::printEigenstratPhased(FILE *out, int numSamples) {
  int numMarkers = Marker::getNumMarkers();
  int numIndivs = P::_allIndivs.length();

  // Print fewer samples than the total (for speed of printing HapMap samples)
  if (numSamples > 0) {
    assert(numSamples <= numIndivs);
    numIndivs = numSamples;
  }

  // What marker index on the chromosome does the next genotype correspond to?
  int chromMarkerIdx = 0;
  // Which chromosome index are we currently on?
  int chromIdx = Marker::getMarker(0)->getChromIdx();

  // curHapChunk: which haplotype chunk are we on? (BITS_PER_CHUNK bit chunks)
  // curChunkIdx: which bit/locus within the chunk are we on?
  for(int m = 0, curHapChunk = 0, curChunkIdx = 0; m < numMarkers;
							  m++, curChunkIdx++) {
    if (curChunkIdx == BITS_PER_CHUNK) {
      curHapChunk++;
      curChunkIdx = 0;
    }

    if (Marker::getLastMarkerNum(chromIdx) == m - 1) {
      // shouldn't be last chrom
      assert(chromIdx < Marker::getNumChroms() - 1);

      // Now on next chromosome; update chunk indices
      if (curChunkIdx != 0) { // markers from prev chrom on current chunk?
	curHapChunk++; // markers for current chrom are on next chunk number
	curChunkIdx = 0;
      }

      chromMarkerIdx = 0; // back to first marker on the new chromosome

      chromIdx = Marker::getMarker(m)->getChromIdx();
    }

    for(int i = 0; i < numIndivs; i++) {
      if (!P::_allIndivs[i]->isPhased())
	// this sample was not phased
	continue;

      for(int h = 0; h < 2; h++) {
      	int hapAllele = P::_allIndivs[i]->getHapAllele(h, curHapChunk,
      						       curChunkIdx, chromIdx,
      						       chromMarkerIdx);
      	fprintf(out, "%d", hapAllele);
      }
    }
    fprintf(out, "\n");
  }
}

// Print a gzip compressed eigenstrat-formated .phgeno.gz file with all phased
// samples to <out>
template <class P>
void PersonIO<P>::printGzEigenstratPhased(gzFile out) {
  int numMarkers = Marker::getNumMarkers();
  int numIndivs = P::_allIndivs.length();

  // What marker index on the chromosome does the next genotype correspond to?
  int chromMarkerIdx = 0;
  // Which chromosome index are we currently on?
  int chromIdx = Marker::getMarker(0)->getChromIdx();

  // curHapChunk: which haplotype chunk are we on? (BITS_PER_CHUNK bit chunks)
  // curChunkIdx: which bit/locus within the chunk are we on?
  for(int m = 0, curHapChunk = 0, curChunkIdx = 0; m < numMarkers;
							  m++, curChunkIdx++) {
    if (curChunkIdx == BITS_PER_CHUNK) {
      curHapChunk++;
      curChunkIdx = 0;
    }

    if (Marker::getLastMarkerNum(chromIdx) == m - 1) {
      // shouldn't be last chrom
      assert(chromIdx < Marker::getNumChroms() - 1);

      // Now on next chromosome; update chunk indices
      if (curChunkIdx != 0) { // markers from prev chrom on current chunk?
	curHapChunk++; // markers for current chrom are on next chunk number
	curChunkIdx = 0;
      }

      chromMarkerIdx = 0; // back to first marker on the new chromosome

      chromIdx = Marker::getMarker(m)->getChromIdx();
    }

    for(int i = 0; i < numIndivs; i++) {
      if (!P::_allIndivs[i]->isPhased())
	// this sample was not phased
	continue;

      for(int h = 0; h < 2; h++) {
	int hapAllele = P::_allIndivs[i]->getHapAllele(h, curHapChunk,
						       curChunkIdx, chromIdx,
						       chromMarkerIdx);
	gzprintf(out, "%d", hapAllele);
      }
    }
    gzprintf(out, "\n");
  }
}

// Print an PLINK-formatted .ped file with all samples to <out>
template <class P>
void PersonIO<P>::printPed(FILE *out){
  int numMarkers = Marker::getNumMarkers();
  int numIndivs = P::_allIndivs.length();

  // Which haplotype chunk are we on?  (A chunk is BITS_PER_CHUNK bits)
  int curHapChunk = 0;
  // Which bit/locus within the chunk are we on?
  int curChunkIdx = 0;

  int sex;
  int geno[2];

  // Iterate through all the individuals
  for (int i = 0; i < numIndivs; i++){
    P *curIndiv = P::_allIndivs[i];
    switch(curIndiv->getSex()){
      case 'M': sex = 1; break;
      case 'F': sex = 2; break;
      default:  sex = 0; break;
    }
    // TODO : figure out how to include the phenotype
    fprintf(out, "%s\t%s\t%d\t%d\t%d\t%d", 
      curIndiv->getPopLabel(), curIndiv->getId(),
      0, 0, sex, 0);

    // What marker index on the chromosome does the next genotype correspond to?
    int chromMarkerIdx = 0;
    // Which chromosome index are we currently on?
    int chromIdx = Marker::getMarker(0)->getChromIdx();

    for (int m = 0; m < numMarkers; m++){
      if (Marker::getLastMarkerNum(chromIdx) == m - 1) {
        // shouldn't be last chrom
        assert(chromIdx < Marker::getNumChroms() - 1);

        // Now on next chromosome; update chunk indices
        if (curChunkIdx != 0) { // markers from prev chrom on current chunk?
          curHapChunk++; // markers for current chrom are on next chunk number
          curChunkIdx = 0;
        }

        chromMarkerIdx = 0; // back to first marker on the new chromosome

        chromIdx = Marker::getMarker(m)->getChromIdx();
      }

      geno[0] = curIndiv->getHapAllele(0, curHapChunk, curChunkIdx,
               chromIdx, chromMarkerIdx);
      geno[1] = curIndiv->getHapAllele(1, curHapChunk, curChunkIdx,
               chromIdx, chromMarkerIdx);      
      fprintf(out, "\t%d\t%d", geno[0] >= 0 ? geno[0]+1 : 0, geno[1] >= 0 ? geno[1] + 1 : 0);

      curChunkIdx++;
      chromMarkerIdx++;
      if (curChunkIdx == BITS_PER_CHUNK) {
        curHapChunk++;
        curChunkIdx = 0;
      }
    }
    fprintf(out, "\n");
  }
}

// Prints a phased ind file for the samples that were phased (those without an
// Ignore label).  If <trioDuoOnly> is true, only prints the ids for parents of
// trios/duos.
template <class P>
void PersonIO<P>::printPhasedIndFile(FILE *out, bool trioDuoOnly) {
  int numIndivs = P::_allIndivs.length();
  for(int ind = 0; ind < numIndivs; ind++) {
    P *thePerson = P::_allIndivs[ind];
    assert(!thePerson->isIgnore()); // sample ignored, should have been deleted

    // if <trioDuoOnly> only print phase for the parents of trios or duos -- no
    // unrelateds
    if (trioDuoOnly && thePerson->isUnrelated())
      continue;

    int idLength = strlen(thePerson->getId());
    for(int h = 0; h < 2; h++) {
      fprintf(out, "   ");
      for(int i = 0; i < P::_maxPersonIdLength - idLength; i++) {
	fprintf(out, " ");
      }
      fprintf(out, "%s%s   %c   %s\n", thePerson->getId(),
	      (h == 0) ? "_A" : "_B", thePerson->getSex(),
	      P::_popLabels[ thePerson->getPopIndex() ]);
    }
  }
}

// Prints IMPUTE2 format .haps file with haplotypes and SNP information
template <class P>
void PersonIO<P>::printImpute2Haps(FILE *out) {
  int numMarkers = Marker::getNumMarkers();
  int numIndivs = P::_allIndivs.length();

  // What marker index on the chromosome does the next genotype correspond to?
  int chromMarkerIdx = 0;
  // Which chromosome index are we currently on?
  int chromIdx = Marker::getMarker(0)->getChromIdx();

  // curHapChunk: which haplotype chunk are we on? (BITS_PER_CHUNK bit chunks)
  // curChunkIdx: which bit/locus within the chunk are we on?
  for(int m = 0, curHapChunk = 0, curChunkIdx = 0; m < numMarkers;
							  m++, curChunkIdx++) {
    if (curChunkIdx == BITS_PER_CHUNK) {
      curHapChunk++;
      curChunkIdx = 0;
    }

    if (Marker::getLastMarkerNum(chromIdx) == m - 1) {
      // shouldn't be last chrom
      assert(chromIdx < Marker::getNumChroms() - 1);

      // Now on next chromosome; update chunk indices
      if (curChunkIdx != 0) { // markers from prev chrom on current chunk?
	curHapChunk++; // markers for current chrom are on next chunk number
	curChunkIdx = 0;
      }

      chromMarkerIdx = 0; // back to first marker on the new chromosome

      chromIdx = Marker::getMarker(m)->getChromIdx();
    }

    // print the first 5 columns that contain SNP information:
    Marker::printImpute2Prefix(out, m);

    // print haplotypes:
    for(int i = 0; i < numIndivs; i++) {
      if (!P::_allIndivs[i]->isPhased())
	// this sample was not phased
	continue;

      for(int h = 0; h < 2; h++) {
	int hapAllele = P::_allIndivs[i]->getHapAllele(h, curHapChunk,
						       curChunkIdx, chromIdx,
						       chromMarkerIdx);
	// the first allele is coded 0, second coded 1 (opposite of Eigenstrat
	// format), so:
	hapAllele ^= 1;
	fprintf(out, " %d", hapAllele);
      }
    }
    fprintf(out, "\n");
  }
}

// Print a gzip compressed IMPUTE2 format .haps file with haplotypes and SNP
// information
template <class P>
void PersonIO<P>::printGzImpute2Haps(gzFile out) {
  int numMarkers = Marker::getNumMarkers();
  int numIndivs = P::_allIndivs.length();

  // What marker index on the chromosome does the next genotype correspond to?
  int chromMarkerIdx = 0;
  // Which chromosome index are we currently on?
  int chromIdx = Marker::getMarker(0)->getChromIdx();

  // curHapChunk: which haplotype chunk are we on? (BITS_PER_CHUNK bit chunks)
  // curChunkIdx: which bit/locus within the chunk are we on?
  for(int m = 0, curHapChunk = 0, curChunkIdx = 0; m < numMarkers;
							  m++, curChunkIdx++) {
    if (curChunkIdx == BITS_PER_CHUNK) {
      curHapChunk++;
      curChunkIdx = 0;
    }

    if (Marker::getLastMarkerNum(chromIdx) == m - 1) {
      // shouldn't be last chrom
      assert(chromIdx < Marker::getNumChroms() - 1);

      // Now on next chromosome; update chunk indices
      if (curChunkIdx != 0) { // markers from prev chrom on current chunk?
	curHapChunk++; // markers for current chrom are on next chunk number
	curChunkIdx = 0;
      }

      chromMarkerIdx = 0; // back to first marker on the new chromosome

      chromIdx = Marker::getMarker(m)->getChromIdx();
    }

    // print the first 5 columns that contain SNP information:
    Marker::printGzImpute2Prefix(out, m);

    // print haplotypes:
    for(int i = 0; i < numIndivs; i++) {
      if (!P::_allIndivs[i]->isPhased())
	// this sample was not phased
	continue;

      for(int h = 0; h < 2; h++) {
	int hapAllele = P::_allIndivs[i]->getHapAllele(h, curHapChunk,
						       curChunkIdx, chromIdx,
						       chromMarkerIdx);
	gzprintf(out, " %d", hapAllele);
      }
    }
    gzprintf(out, "\n");
  }
}

// Prints IMPUTE2 format .sample file
template <class P>
void PersonIO<P>::printImpute2SampleFile(FILE *out, bool trioDuoOnly) {
  // Print header lines:
  fprintf(out, "ID_1 ID_2 missing\n");
  fprintf(out, "0 0 0\n");

  int numIndivs = P::_allIndivs.length();
  for(int ind = 0; ind < numIndivs; ind++) {
    P *thePerson = P::_allIndivs[ind];
    assert(!thePerson->isIgnore()); // sample ignored, should have been deleted

    // if <trioDuoOnly> only print phase for the parents of trios or duos -- no
    // unrelateds
    if (trioDuoOnly && thePerson->isUnrelated())
      continue;

    // Print family id (using printf substring trick) and individual id
    int familyIdLength = thePerson->getFamilyIdLength();
    if (familyIdLength > 0) {
      fprintf(out, "%.*s %s 0\n", familyIdLength, thePerson->getId(),
	      &thePerson->getId()[familyIdLength+1] );
    }
    else {
      fprintf(out, "%s %s 0\n", thePerson->getId(), thePerson->getId());
    }
  }
}

// explicitly instantiate PersionIO with the Person classes so we don't get
// linker errors
template class PersonIO<PersonBits>;
template class PersonIO<PersonNorm>;
