-----------------
[![GitHub license](https://img.shields.io/badge/license-apache-green.svg?style=flat)](https://www.apache.org/licenses/LICENSE-2.0)
[![Version](https://img.shields.io/badge/version-0.0.1-red.svg?style=flat)]()
## Introduction

Valet is an efficient approach to orchestration of host and remote shared memory for improving performance of memory intensive workloads.
The original paper describing Valet appeared in MEMSYS'20.

## Installation

1. Install MLNX_OFED driver
```bash
wget http://www.mellanox.com/downloads/ofed/MLNX_OFED-3.1-1.0.3/MLNX_OFED_LINUX-3.1-1.0.3-ubuntu14.04-x86_64.tgz .
tar -xvf MLNX_OFED_LINUX-3.1-1.0.3-ubuntu14.04-x86_64.tgz
cd ~/MLNX_OFED_LINUX-3.1-1.0.3-ubuntu14.04-x86_64
sudo ./mlnxofedinstall --add-kernel-support --without-fw-update --force
```

2. Install Valet

(for both client and server node)
```bash
cd ~/valet/setup
./install.sh
```

3. Run Valet

Assume that IP address is 100.10.10.0(client)/100.10.10.1(peer1)/100.10.10.2(peer2)/100.10.10.3(peer3) and disk partion is /dev/sda3

(Modify portal list)

vi valet/setup/portal.list
```bash
3 -> number of peer nodes
100.10.10.1:9999 -> IPaddr and Port
100.10.10.2:9999
100.10.10.3:9999
```
*Important note : portal.list must be same in all peer node. In other words, the order of peer nodes in the list should be same. 


(Peer nodes: repeat this for peer2 and 3)
```bash
cd ~/valet/setup/
~/ib_setup.sh 100.10.10.1
cd ~/valet/daemon
./daemon -a 100.10.10.1 -p 9999 -i "/users/username/valet/setup/portal.list"
```

(Client node)
```bash
cd ~/valet/setup/
~/ib_setup.sh 100.10.10.0
sudo swapoff /dev/sda3 -> we swap off current 1st priority swap partition(It may vary on systems).
sudo ~/bd_setup.sh 0
```

(Check)

sudo swapon -s (on client node)

## Setting parameters

valet/bd/rdmabox.h

- SERVER_SELECT_NUM [number]

  This number should be equal to or less than the number of peer nodes.(e.g. if 3 peers, this should be <=3)

- NUM_REPLICA [number]

  Number of replicated copies on peer nodes.
```bash
#define SERVER_SELECT_NUM 2
#define NUM_REPLICA 1
```

valet/bd/valet_drv.h

- SWAPSPACE_SIZE_G [number]

  Set total size of swapspace for valet in GB.

- BACKUP_DISK [string]

  Set disk partition path is diskbackup is used. 

  (By default, Valet does not use diskbackup. Use replication)

```bash
#define SWAPSPACE_SIZE_G        40
#define BACKUP_DISK     "/dev/sda4"
```

## Supported Platforms

Tested environment:

OS : Ubuntu 14.04(kernel 3.13.0)

RDMA NIC driver: MLNX_OFED 3.1

Hardware : Infiniband, Mellanox ConnectX-3/4, disk partition(Only if diskbackup option is enabled.)


Note that code supports up to Ubuntu 18.04 kernel 4.15 but not tested.

## Status

The code is provided as is, without warranty or support. If you use our code, please cite:
```
@inproceedings{bae2020valet,
  title={Efficient Orchestration of Host and Remote Shared Memory for Memory Intensive Workloads," The International Symposium on Memory Systems},
    author={Bae, Juhyun and Liu, Ling and Su, Gong and Iyengar, Arun and Wu, Yanzhao},
      booktitle={Proceedings of The International Symposium on Memory Systems},
        year={2020}
}
```
