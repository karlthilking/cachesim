#!/bin/bash

CWD=$(echo $PWD)
CPUPATH=$(echo /sys/devices/system/cpu)
cd $CPUPATH

NCPUS=$(echo $(ls | sort | uniq | grep 'cpu[0-9+]' | wc -l))

L1D_CACHE_SIZE=''
L1D_BLOCK_SIZE=''
L1D_ASSOC=''
L1I_CACHE_SIZE=''
L1I_BLOCK_SIZE=''
L1I_ASSOC=''
L1_CACHE_SIZE=''
L1_BLOCK_SIZE=''
L1_ASSOC=''

L2D_CACHE_SIZE=''
L2D_BLOCK_SIZE=''
L2D_ASSOC=''
L2I_CACHE_SIZE=''
L2I_BLOCK_SIZE=''
L2I_ASSOC=''
L2_CACHE_SIZE=''
L2_BLOCK_SIZE=''
L2_ASSOC=''

for (( i=0; i<NCPUS; i+=1 ));
do
        CACHEPATH=$(echo $CPUPATH/cpu$i/cache)
        cd $CACHEPATH
        NCACHES=$(echo $(ls | grep 'index[0-9]' | sort | uniq | wc -l))
        for (( j=0; j<NCACHES; j+=1 ));
        do
                INDEXPATH=$CACHEPATH/index$j
                CACHE_SIZE=$(cat $INDEXPATH/size)
                BLOCK_SIZE=$(cat $INDEXPATH/coherency_line_size)
                NUM_SETS=$(cat $INDEXPATH/number_of_sets)
                CACHE_LEVEL=$(cat $INDEXPATH/level)
                CACHE_ASSOC=$(cat $INDEXPATH/ways_of_associativity)
                CACHE_TYPE=$(cat $INDEXPATH/'type')
                SHARE_LIST=$(cat $INDEXPATH/shared_cpu_list)
                CACHE_ID=$(cat $INDEXPATH/id)

                if [[ $CACHE_LEVEL = "1" ]]; then
                        if [[ $CACHE_TYPE == "Data" ]]; then
                                L1D_CACHE_SIZE=$CACHE_SIZE
                                L1D_BLOCK_SIZE=$BLOCK_SIZE
                                L1D_ASSOC=$CACHE_ASSOC
                        elif [[ $CACHE_TYPE == "Instruction" ]]; then
                                L1I_CACHE_SIZE=$CACHE_SIZE
                                L1I_BLOCK_SIZE=$BLOCK_SIZE
                                L1I_ASSOC=$CACHE_ASSOC
                        elif [[ $CACHE_TYPE == "Unified" ]]; then
                                L1_CACHE_SIZE=$CACHE_SIZE
                                L1_BLOCK_SIZE=$BLOCK_SIZE
                                L1_ASSOC=$CACHE_ASSOC
                        fi
                elif [[ $CACHE_LEVEL = "2" ]]; then
                        if [[ $CACHE_TYPE == "Unified" ]]; then
                                L2_CACHE_SIZE=$CACHE_SIZE
                                L2_BLOCK_SIZE=$BLOCK_SIZE
                                L2_ASSOC=$CACHE_ASSOC
                        elif [[ $CACHE_TYPE == "Data" ]]; then
                                L2D_CACHE_SIZE=$CACHE_SIZE
                                L2D_BLOCK_SIZE=$BLOCK_SIZE
                                L2D_ASSOC=$CACHE_ASSOC
                        elif [[ $CACHE_TYPE == "Instruction" ]]; then
                                L2I_CACHE_SIZE=$CACHE_SIZE
                                L2I_BLOCK_SIZE=$BLOCK_SIZE
                                L2I_ASSOC=$CACHE_ASSOC
                        fi
                elif [[ $CACHE_LEVEL == "3" ]]; then
                        L3_CACHE_SIZE=$CACHE_SIZE
                        L3_BLOCK_SIZE=$BLOCK_SIZE
                        L3_ASSOC=$CACHE_ASSOC
                fi
        done
done

cd $CWD
if [[ -n $(find . -type d -name 'bin') ]]; then
        cd ./include
elif [[ -n $(find .. -type d -name 'bin') ]]; then
        cd ../include
fi

if [[ -n "$L1_CACHE_SIZE" ]]; then
        unit="${L1_CACHE_SIZE: -1}"
        L1_CACHE_SIZE="${L1_CACHE_SIZE%?}"
        case "$unt" in
                K) L1_CACHE_SIZE=$((L1_CACHE_SIZE << 10)) ;;
                M) L1_CACHE_SIZE=$((L1_CACHE_SIZE << 20)) ;;
                G) L1_CACHE_SIZE=$((L1_CACHE_SIZE << 30)) ;;
        esac
fi

if [[ -n "$L1I_CACHE_SIZE" ]]; then
        unit="${L1I_CACHE_SIZE: -1}"
        L1I_CACHE_SIZE="${L1I_CACHE_SIZE%?}"
        case "$unit" in
                K) L1I_CACHE_SIZE=$((L1I_CACHE_SIZE << 10)) ;;
                M) L1I_CACHE_SIZE=$((L1I_CACHE_SIZE << 20)) ;;
                G) L1I_CACHE_SIZE=$((L1I_CACHE_SIZE << 30)) ;;
        esac
fi

if [[ -n "$L1D_CACHE_SIZE" ]]; then
        unit="${L1D_CACHE_SIZE: -1}"
        L1D_CACHE_SIZE="${L1D_CACHE_SIZE%?}"
        case "$unit" in
                K) L1D_CACHE_SIZE=$((L1D_CACHE_SIZE << 10)) ;;
                M) L1D_CACHE_SIZE=$((L1D_CACHE_SIZE << 20)) ;;
                G) L1D_CACHE_SIZE=$((L1D_CACHE_SIZE << 30)) ;;
        esac
fi

if [[ -n "$L2_CACHE_SIZE" ]]; then
        unit="${L2_CACHE_SIZE: -1}"
        L2_CACHE_SIZE="${L2_CACHE_SIZE%?}"
        case "$unit" in
                K) L2_CACHE_SIZE=$((L2_CACHE_SIZE << 10)) ;;
                M) L2_CACHE_SIZE=$((L2_CACHE_SIZE << 20)) ;;
                G) L2_CACHE_SIZE=$((L2_CACHE_SIZE << 30)) ;;
        esac
fi

if [[ -n "$L3_CACHE_SIZE" ]]; then
        unit="${L3_CACHE_SIZE: -1}"
        L3_CACHE_SIZE="${L3_CACHE_SIZE%?}"
        case "$unit" in
                K) L3_CACHE_SIZE=$((L3_CACHE_SIZE << 10)) ;;
                M) L3_CACHE_SIZE=$((L3_CACHE_SIZE << 20)) ;;
                G) L3_CACHE_SIZE=$((L3_CACHE_SIZE << 30)) ;;
        esac
fi

CACHE_LEVELS=''
if [[ -n "$L2_CACHE_SIZE" || -n "$L2I_CACHE_SIZE" ]]; then
        if [[ -n "$L3_CACHE_SIZE" ]]; then
                CACHE_LEVELS='3'
        else
                CACHE_LEVELS='2'
        fi
else
        CACHE_LEVELS='1'
fi

echo '#ifndef __CACHE_PARAMS_HPP__'                      > params.hpp
echo '#define __CACHE_PARAMS_HPP__'                      >> params.hpp
echo '#include <memory>'                                 >> params.hpp
echo '#include "cachesim.hpp"'                           >> params.hpp
echo ''                                                  >> params.hpp
echo 'constexpr u8 nr_levels = '$CACHE_LEVELS';'         >> params.hpp
echo 'constexpr u8 nr_cpus = '$NCPUS';'                  >> params.hpp
echo ''                                                  >> params.hpp
if [[ -n "$L1_CACHE_SIZE" ]]; then
  echo 'constexpr u64 l1_cache_size = '$L1_CACHE_SIZE';' >> params.hpp
  echo 'constexpr u16 l1_block_size = '$L1_BLOCK_SIZE';' >> params.hpp
  echo 'constexpr u16 l1_associativity = '$L1_ASSOC';'   >> params.hpp
  echo 'constexpr cache_type l1_type = UNIFIIED;'        >> params.hpp
  echo '' >> params.hpp
else
  echo 'constexpr int l1_cache_size = '-1';'             >> params.hpp
  echo 'constexpr int l1_block_size = '-1';'             >> params.hpp
  echo 'constexpr int l1_associativity = '-1';'          >> params.hpp
  echo '' >> params.hpp
fi
if [[ -n "$L1I_CACHE_SIZE" ]]; then
  echo 'constexpr u64 l1i_cache_size = '$L1I_CACHE_SIZE';' >> params.hpp
  echo 'constexpr u16 l1i_block_size = '$L1I_BLOCK_SIZE';' >> params.hpp
  echo 'constexpr u16 l1i_associativity = '$L1I_ASSOC';'   >> params.hpp
  echo 'constexpr cache_type l1_type = SPLIT;'             >> params.hpp
  echo '' >> params.hpp
else
  echo 'constexpr int l1i_cache_size = '-1';'              >> params.hpp
  echo 'constexpr int l1i_block_size = '-1';'              >> params.hpp
  echo 'constexpr int l1i_associativity = '-1';'           >> params.hpp
  echo '' >> params.hpp
fi
if [[ -n "$L1D_CACHE_SIZE" ]]; then
  echo 'constexpr u64 l1d_cache_size = '$L1D_CACHE_SIZE';' >> params.hpp
  echo 'constexpr u16 l1d_block_size = '$L1D_BLOCK_SIZE';' >> params.hpp
  echo 'constexpr u16 l1d_associativity = '$L1D_ASSOC';'   >> params.hpp
  echo '' >> params.hpp
else
  echo 'constexpr int l1d_cache_size = '-1';'              >> params.hpp
  echo 'constexpr int l1d_block_size = '-1';'              >> params.hpp
  echo 'constexpr int l1d_associativity = '-1';'           >> params.hpp
  echo '' >> params.hpp
fi
echo '' >> params.hpp
if [[ -n "$L2_CACHE_SIZE" ]]; then
  echo 'constexpr u64 l2_cache_size = '$L2_CACHE_SIZE';' >> params.hpp
  echo 'constexpr u16 l2_block_size = '$L2_BLOCK_SIZE';' >> params.hpp
  echo 'constexpr u16 l2_associativity = '$L2_ASSOC';'   >> params.hpp
  echo 'constexpr cache_type l2_type = UNIFIED;'         >> params.hpp
  echo '' >> params.hpp
else
  echo 'constexpr int l2_cache_size = -1;'             >> params.hpp
  echo 'constexpr int l2_block_size = -1;'             >> params.hpp
  echo 'constexpr int l2_associativity = -1;'          >> params.hpp
fi
if [[ -n "$L2I_CACHE_SIZE" ]]; then
  echo 'constexpr u64 l2i_cache_size = '$L2I_CACHE_SIZE';' >> params.hpp
  echo 'constexpr u16 l2i_block_size = '$L2I_BLOCK_SIZE';' >> params.hpp
  echo 'constexpr u16 l2i_associativity = '$L2I_ASSOC';'   >> params.hpp
  echo 'constexpr cache_type l2_type = SPLIT;'             >> params.hpp
  echo '' >> params.hpp
else
  echo 'constexpr int l2i_cache_size = -1;'               >> params.hpp
  echo 'constexpr int l2i_block_size = -1;'               >> params.hpp
  echo 'constexpr int l2i_associativity = -1;'            >> params.hpp
  echo '' >> params.hpp
fi
if [[ -n "$L2D_CACHE_SIZE" ]]; then
  echo 'constexpr u64 l2d_cache_size = '$L2D_CACHE_SIZE';' >> params.hpp
  echo 'constexpr u16 l2d_block_size = '$L2D_BLOCK_SIZE';' >> params.hpp
  echo 'constexpr u16 l2d_associativity = '$L2D_ASSOC';'   >> params.hpp
  echo '' >> params.hpp
else
  echo 'constexpr int l2d_cache_size = -1;'               >> params.hpp
  echo 'constexpr int l2d_block_size = -1;'               >> params.hpp
  echo 'constexpr int l2d_associativity = -1;'            >> params.hpp
  echo '' >> params.hpp
fi
echo '' >> params.hpp
if [[ -n "$L3_CACHE_SIZE" ]]; then
  echo 'constexpr u64 l3_cache_size = '$L3_CACHE_SIZE';' >> params.hpp
  echo 'constexpr u16 l3_block_size = '$L3_BLOCK_SIZE';' >> params.hpp
  echo 'constexpr u16 l3_associativity = '$L3_ASSOC';'   >> params.hpp
  echo 'constexpr cache_type l3_type = UNIFIED;'         >> params.hpp
  echo '' >> params.hpp
else
  echo 'constexpr int l3_cache_size = -1;'               >> params.hpp
  echo 'constexpr u16 l3_block_size = -1;'               >> params.hpp
  echo 'constexpr u16 l3_associativity = -1;'            >> params.hpp
  echo 'constexpr cache_type l3_type = NONE;'            >> params.hpp
  echo '' >> params.hpp
fi
echo '#endif // __CACHE_PARAMS_HPP__'                    >> params.hpp
