JDKSAVDECC = ../jdksavdecc-c

all:
	gcc -I$(JDKSAVDECC)/include -I. -no-pie -g -o avb-user avb-user.c mrp_client.c maap_client.c avpdu.c avb_log.c utils.c gptp_client.c gptp_link.c avtp.c msrp_talker.c msrp_listener.c msrp_domain.c avdecc_adp.c avdecc_aecp.c avdecc_acmp.c avdecc_peers.c avdecc_probe.c avb_status.c descriptor.c $(JDKSAVDECC)/libjdksavdecc-c.a -lpthread -lrt

clean:
	rm -f *.o avb-user