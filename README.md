# genetio
Library for I/O and storage of genetic data

Supported input formats:

  * PLINK binary ped (.bed/.bim/.fam)
  * Eigenstrat
  * Packed Ancestrymap

Supported output formats for phased haplotype data:

  * Eigenstrat (.phgeno/.phsnp/.phind)
  * Gzipped Eigenstrat
  * Impute2
  * Gzipped Imput2

Output for unphased genotypes available in Eigenstrat format

Internal storage formats:

  * Each genotype stored in three bits, implemented in PersonBits---enables rapid processing of multiple SNPs at a time using 64-bit unsigned ints
  * A more standard format with an integer per allele at each marker, implemented in PersonNorm

Coming soon: support for VCF input
