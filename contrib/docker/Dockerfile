FROM ubuntu:16.04

RUN apt-get -y update
RUN apt-get -y install git-core build-essential make && git clone -b dev https://github.com/Huawei-PaaS/firmament /firmament
RUN bash -c 'source /firmament/include/pkglist.Ubuntu-16.04 && apt-get -y install ${BASE_PKGS} ${COMPILER_PKGS} ${GOOGLE_PKGS} ${BOOST_PKGS} ${MISC_PKGS} ${HDFS_PKGS} ${PION_PKGS}'
RUN mkdir /var/log/firmament && cd /firmament && make && cd build && make -j12
RUN apt-get clean
