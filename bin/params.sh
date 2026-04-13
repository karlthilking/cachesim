#!/bin/bash

CWD=$(echo $PWD)
CPUPATH=$(echo /sys/devices/system/cpu)
cd $CPUPATH

NCPUS=$(echo $(ls | sort | uniq | grep 'cpu[0-9+]' | wc -l))

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
cd ./include/

unit="${L1I_CACHE_SIZE: -1}"
L1I_CACHE_SIZE="${L1I_CACHE_SIZE%?}"
case "$unit" in
        K) L1I_CACHE_SIZE=$((L1I_CACHE_SIZE * 1024)) ;;
        M) L1I_CACHE_SIZE=$((L1I_CACHE_SIZE * 1024 * 1024)) ;;
        G) L1I_CACHE_SIZE=$((L1I_CACHE_SIZE * 1024 * 1024 * 1024)) ;;
esac

unit="${L1D_CACHE_SIZE: -1}"
L1D_CACHE_SIZE="${L1D_CACHE_SIZE%?}"
case "$unit" in
        K) L1D_CACHE_SIZE=$((L1D_CACHE_SIZE * 1024)) ;;
        M) L1D_CACHE_SIZE=$((L1D_CACHE_SIZE * 1024 * 1024)) ;;
        G) L1D_CACHE_SIZE=$((L1D_CACHE_SIZE * 1024 * 1024 * 1024)) ;;
esac

unit="${L2_CACHE_SIZE: -1}"
L2_CACHE_SIZE="${L2_CACHE_SIZE%?}"
case "$unit" in
        K) L2_CACHE_SIZE=$((L2_CACHE_SIZE * 1024)) ;;
        M) L2_CACHE_SIZE=$((L2_CACHE_SIZE * 1024 * 1024)) ;;
        G) L2_CACHE_SIZE=$((L2_CACHE_SIZE * 1024 * 1024 * 1024)) ;;
esac

unit="${L3_CACHE_SIZE: -1}"
L3_CACHE_SIZE="${L3_CACHE_SIZE%?}"
case "$unit" in
        K) L3_CACHE_SIZE=$((L3_CACHE_SIZE * 1024)) ;;
        M) L3_CACHE_SIZE=$((L3_CACHE_SIZE * 1024 * 1024)) ;;
        G) L3_CACHE_SIZE=$((L3_CACHE_SIZE * 1024 * 1024 * 1024)) ;;
esac

echo '#ifndef __CACHE_PARAMS_HPP__'                      > params.hpp
echo '#define __CACHE_PARAMS_HPP__'                      >> params.hpp
echo '#include "cachesim.hpp"'                           >> params.hpp
echo ''                                                  >> params.hpp
echo 'constexpr u32 l1i_cache_size = '$L1I_CACHE_SIZE';' >> params.hpp
echo 'constexpr u16 l1i_block_size = '$L1I_BLOCK_SIZE';' >> params.hpp
echo 'constexpr u16 l1i_associativity = '$L1I_ASSOC';'   >> params.hpp
echo ''                                                  >> params.hpp
echo 'constexpr u32 l1d_cache_size = '$L1D_CACHE_SIZE';' >> params.hpp
echo 'constexpr u16 l1d_block_size = '$L1D_BLOCK_SIZE';' >> params.hpp
echo 'constexpr u16 l1d_associativity = '$L1D_ASSOC';'   >> params.hpp
echo ''                                                  >> params.hpp
echo 'constexpr u32 l2_cache_size = '$L2_CACHE_SIZE';'   >> params.hpp
echo 'constexpr u16 l2_block_size = '$L2_BLOCK_SIZE';'   >> params.hpp
echo 'constexpr u16 l2_associativity = '$L2_ASSOC';'     >> params.hpp
echo ''                                                  >> params.hpp
echo 'constexpr u32 l3_cache_size = '$L3_CACHE_SIZE';'   >> params.hpp
echo 'constexpr u16 l3_block_size = '$L3_BLOCK_SIZE';'   >> params.hpp
echo 'constexpr u16 l3_associativity = '$L3_ASSOC';'    >> params.hpp
echo ''                                                  >> params.hpp
echo '#endif // __CACHE_PARAMS_HPP__'                    >> params.hpp
