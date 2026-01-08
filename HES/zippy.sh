#!/bin/bash
tempdir=$(mktemp -t -d foo.zip.XXXXXX)
dculist=("$@")
file=""
for ((i=0; i < $#; i++))
{
     if [ $i -eq 0 ];then
	file=${dculist[$i]}
     else
     	#file="/home/tarz_ragavan/Amol/PMESH_NMS_V1.9_15072023_DEV_TEST/logs_1/"${dculist[$i]}
     	file1="$file/"${dculist[$i]}
     	#echo $file1
     	if test -f "$file1";then
          cp $file1 $tempdir
     	fi	     
     fi
}

if [ -z "$(ls -A $tempdir)" ]; then
 echo "Empty Directory"  
else
 #echo "Not Empty"
 #cd $tempdir && zip -r /home/hes/foo.zip ./*
 cd $tempdir && zip  -9 -r -q  /home/tarz_ragavan/AMI_INTELLISMART/NEW_HES/FINAL_HES_NEW_DYNAMIC_MERGE/foo.zip ./*
 echo "Success"
fi

rm -rf ${tempdir}
cd ~
