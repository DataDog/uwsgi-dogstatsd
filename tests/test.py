import select
import socket
import sys
import urllib2
from time import time, sleep

UDP_SOCKET_TIMEOUT = 5

class Test(object):
    def __init__(self, data):
        self.data = data

    def run(self):
        return

class Data(object):
    """
    The data aggregated from the uwsgi app
    """

    def __init__(self):
        self.data = {}

    def parse_packet(self, packet):
        tags = None
        metadata = packet.split('|')
        if (len(metadata) < 2):
            raise Exception('Unparseable metric packet: %s' % packet)
    
        name_value = metadata[0].split(':')
        metric_type = metadata[1]
        if (len(metadata) == 3):
            tags = metadata[2].split(',')
            if (len(tags) < 1 or not tags[0].startswith('#')):
                raise Exception('Unparseable metric packet: %s' % packet)
            tags[0] = tags[0][1:]

        if (len(name_value) != 2): 
            raise Exception('Unparseable metric packet: %s' % packet)

        
        metric = {
            'name': name_value[0],
            'value': name_value[1],
            'metric_type': metric_type,
            'tags': tags
        }
        print metric
        self.data[name_value[0]] = metric

    def new_packets(self, packets):
        packets = unicode(packets, 'utf-8', errors='replace')
        for packet in packets.splitlines():
            if not packet.strip():
                continue

            self.parse_packet(packet)
        
    def get_data(self):
        return self.data

class Server(object):
    """
    The process which will listen on the statd port
    """
    config = {
        'host': 'localhost',
        'port': 8125
    }

    def __init__(self, data):
        self.data = data
        self.buffer_size = 1024 * 8
        self.address = (self.config['host'], self.config['port'])

    def run(self):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.setblocking(0)
        try:
            self.socket.bind(self.address)
        except socket.gaierror:
            if self.address[0] == 'localhost':
                log.warning("Warning localhost seems undefined in your host file, using 127.0.0.1 instead")
                self.address = ('127.0.0.1', self.address[1])
                self.socket.bind(self.address)

        print "Listening on host & port: %s" % str(self.address)

        sock = [self.socket]
        select_select = select.select
        timeout = UDP_SOCKET_TIMEOUT
  
        self.running = True
        while self.running:
            try:
                ready = select_select(sock, [], [], timeout)
                if ready[0]:
                    message = self.socket.recv(self.buffer_size)
                    self.data.new_packets(message)
            except (KeyboardInterrupt, SystemExit):
                break
            except Exception:
                print 'Error receiving datagram'

    def stop(self):
        sef.running = False

def main():
    data = Data()
    server = Server(data)
    test = Test(data)
    try:
        server.run()
        test.run()
    except:
        print 'Error: unable to start thread'

if __name__ == '__main__':
    sys.exit(main())
