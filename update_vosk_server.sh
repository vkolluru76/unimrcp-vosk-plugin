set +e
sudo rm -rf /opt/unimrcp \
    && sudo rm -rf /opt/unimrcp-deps \
    && sudo mkdir -p /opt/unimrcp-deps \
    && sudo wget http://www.unimrcp.org/project/component-view/unimrcp-deps-1-6-0-tar-gz/download -O - | sudo tar -xz --strip-components=1 --directory /opt/unimrcp-deps \
    && cd /opt/unimrcp-deps && yes y | sudo ./build-dep-libs.sh \
    && sudo git clone -b updated-vosk-plugin https://github.com/vkolluru76/unimrcp-vosk-plugin.git /opt/unimrcp \
    && cd /opt/unimrcp/ \
    && sudo sed -i 's|$(HOME)/travis/install|/opt/vosk-api|g' /opt/unimrcp/plugins/vosk-recog/Makefile.am \
    && sudo sed -i 's|$(VOSK_HOME)/include|$(VOSK_HOME)/src|g' /opt/unimrcp/plugins/vosk-recog/Makefile.am \
    && sudo sed -i 's|$(VOSK_HOME)/lib|$(VOSK_HOME)/src|g' /opt/unimrcp/plugins/vosk-recog/Makefile.am \
    && sudo sed -i 's|$(VOSK_HOME)|/opt/vosk-api|g' /opt/unimrcp/plugins/vosk-recog/Makefile.am \
    && sudo ./bootstrap \
    && sudo ./configure \
    && VOSK_HOME=/opt/vosk-api sudo make -j $(nproc) \
    && sudo make install \
    && sudo /sbin/ldconfig \
    && sudo rm -rf /opt/unimrcp-deps \
    && sudo rm -rf /opt/unimrcp \
    && sudo rm -rf /root/.cache


# sofia
cd
rm -rf sofia-sip
git clone --single-branch https://github.com/freeswitch/sofia-sip.git
sudo apt-get install -y libssl-dev
( cd ./sofia-sip && ./bootstrap.sh -j && ./configure && make && sudo make install )
rm -rf sofia-sip


#change the sip server port to 8070 for the unimrcp as 8060 will be handled by freeswitch
sudo sed -i 's|<sip-port>8060</sip-port>|<sip-port>8070</sip-port>|g' /usr/local/unimrcp/conf/unimrcpserver.xml

#modify the logging output to FILE instead of console
sudo sed -i 's|<output>CONSOLE</output>|<output>FILE</output>|g' /usr/local/unimrcp/conf/logger.xml

#modify the unimrcp test client umc to use the server port 8070 instead of the default 8060
sudo sed -i '14s|<server-port>8060</server-port>|<server-port>8070</server-port>|' /usr/local/unimrcp/conf/client-profiles/unimrcp.xml

cbc-restart

