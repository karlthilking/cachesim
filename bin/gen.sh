#!/bin/bash

ORIGIN=$(echo $PWD)
cd $(echo /sys/devices/system/cpu)

NCPUS=$(echo $(ls | sort | uniq | grep 'cpu[0-9+]' | wc -l))

for (( i=0; i<NCPUS; i+=1 ));
do
    cd $(echo /sys/devices/system/cpu/cpu$i/cache)

    NCACHES=$(echo $(ls | grep 'index[0-9]' | sort | uniq | wc -l))
    for (( j=0; j<NCACHES; j+=1 ));
    do
        SIZE=$(cat index$j/size)
        BLOCK_SIZE=$(cat index$j/coherency_line_size)
        ASSOC=$(cat index$j/ways_of_associativity)
        LEVEL=$(cat index$j/level)
        TYPE=$(cat index$j/'type')

        if [[ $LEVEL = "1" && $TYPE = "Instruction" ]]; then
            L1I_SIZE=$SIZE
            L1I_BLOCK_SIZE=$BLOCK_SIZE
            L1I_ASSOC=$ASSOC
        elif [[ $LEVEL = "1" && $TYPE = "Data" ]]; then
            L1D_SIZE=$SIZE
            L1D_BLOCK_SIZE=$BLOCK_SIZE
            L1D_ASSOC=$ASSOC
        elif [[ $LEVEL = "2" ]]; then
            L2_SIZE=$SIZE
            L2_BLOCK_SIZE=$BLOCK_SIZE
            L2_ASSOC=$ASSOC
        elif [[ $LEVEL = "3" ]]; then
            L3_SIZE=$SIZE
            L3_BLOCK_SIZE=$BLOCK_SIZE
            L3_ASSOC=$ASSOC
        fi
    done
done

cd $ORIGIN
if [[ -n $(find . -type d -name 'bin') ]]; then
    cd ./include
elif [[ -n $(find .. -type d -name 'bin') ]]; then
    cd ../include
fi

unit="${L1I_SIZE: -1}"
L1I_SIZE="${L1I_SIZE%?}"
case "$unit" in
    K) L1I_SIZE=$((L1I_SIZE << 10)) ;;
    M) L1I_SIZE=$((L1I_SIZE << 20)) ;;
    G) L1I_SIZE=$((L1I_SIZE << 30)) ;;
esac

unit="${L1D_SIZE: -1}"
L1D_SIZE="${L1D_SIZE%?}"
case "$unit" in
    K) L1D_SIZE=$((L1D_SIZE << 10)) ;;
    M) L1D_SIZE=$((L1D_SIZE << 20)) ;;
    G) L1D_SIZE=$((L1D_SIZE << 30)) ;;
esac

unit="${L2_SIZE: -1}"
L2_SIZE="${L2_SIZE%?}"
case "$unit" in
    K) L2_SIZE=$((L2_SIZE << 10)) ;;
    M) L2_SIZE=$((L2_SIZE << 20)) ;;
    G) L2_SIZE=$((L2_SIZE << 30)) ;;
esac

unit="${L3_SIZE: -1}"
L3_SIZE="${L3_SIZE%?}"
case "$unit" in
    K) L3_SIZE=$((L3_SIZE << 10)) ;;
    M) L3_SIZE=$((L3_SIZE << 20)) ;;
    G) L3_SIZE=$((L3_SIZE << 30)) ;;
esac

echo '#ifndef __CACHE_PARAMS_HPP__'                             > params.hpp
echo '#define __CACHE_PARAMS_HPP__'                             >> params.hpp
echo ''                                                         >> params.hpp
echo 'namespace cachesim {'                                     >> params.hpp
echo 'constexpr size_t  ncpus           = '$NCPUS';'            >> params.hpp
echo 'constexpr size_t  l1i_size        = '$L1I_SIZE';'         >> params.hpp
echo 'constexpr size_t  l1i_blk_size    = '$L1I_BLOCK_SIZE';'   >> params.hpp
echo 'constexpr int     l1i_assoc       = '$L1I_ASSOC';'        >> params.hpp
echo 'constexpr size_t  l1d_size        = '$L1D_SIZE';'         >> params.hpp
echo 'constexpr size_t  l1d_blk_size    = '$L1D_BLOCK_SIZE';'   >> params.hpp
echo 'constexpr int     l1d_assoc       = '$L1D_ASSOC';'        >> params.hpp
echo 'constexpr size_t  l2_size         = '$L2_SIZE';'          >> params.hpp
echo 'constexpr size_t  l2_blk_size     = '$L2_BLOCK_SIZE';'    >> params.hpp
echo 'constexpr int     l2_assoc        = '$L2_ASSOC';'         >> params.hpp
echo 'constexpr size_t  l3_size         = '$L3_SIZE';'          >> params.hpp
echo 'constexpr size_t  l3_blk_size     = '$L3_BLOCK_SIZE';'    >> params.hpp
echo 'constexpr int     l3_assoc        = '$L3_ASSOC';'         >> params.hpp
echo '} // cachesim'                                            >> params.hpp
echo '#endif // __CACHE_PARAMS_HPP__'                           >> params.hpp

