# RCC

# Prepare

1. Install packages.
```
sudo apt-get update  
sudo apt-get install -y gcc python3 python3-pip python-dateutil python3-dateutil bash sudo git gnuplot bison flex libreadline6 libreadline6-dev linux-tools-generic zlib1g-dev vim autoconf linux-tools-`uname -r` build-essential libreadline-dev pkg-config libmysqlclient-dev libpq-dev libtool libaio1 libaio-dev cmake libncurses5 libncurses5-dev bison libssl-dev openssl

```

# PostgreSQL
```
cd PostgreSQL/Figure_12_the_CDF_of_version_chain_length
bash ./script_main.sh
```

# MySQL
```
cd MySQL
bash build.sh
```
basic commands:
```
bash run_server.sh
bash shutdown.sh
```

run experiments in paper:
```
bash fig4_varying_transaction_length.sh
bash fig5_varying_hotspot_position.sh
bash fig6_varying_client_threads.sh
bash fig7_varying_warehouse_count.sh
```

 