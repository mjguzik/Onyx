/*
* Copyright (c) 2020 Pedro Falcato
* This file is part of Onyx, and is released under the terms of the MIT License
* check LICENSE at the root directory for more information
*/

#include <gtest/gtest.h>
#include <limits>

#include <sys/socket.h>

#include <netinet/ip.h>
#include <arpa/inet.h>

TEST(Udp, Inet4CorkWorks)
{
	int sock = socket(AF_INET, SOCK_DGRAM, 0);

	ASSERT_NE(sock, -1);

	sockaddr_in sa = {};
	sa.sin_addr.s_addr = INADDR_ANY;
	sa.sin_port = htons(1066);
	sa.sin_family = AF_INET;

	ASSERT_NE(bind(sock, (const sockaddr *) &sa, sizeof(sa)), -1);

#if 0
	int off = IP_PMTUDISC_DONT;
	ASSERT_NE(setsockopt(sock, SOL_IP, IP_MTU_DISCOVER, &off, sizeof(off)), -1);
#endif

	void *ptr = new char[std::numeric_limits<uint16_t>::max()];

	constexpr auto max_size = UINT16_MAX - 8 /* sizeof udphdr */ - sizeof(iphdr);
	iovec v;
	v.iov_base = ptr;
	v.iov_len = max_size / 16;

	msghdr msg;
	msg.msg_control = nullptr;
	msg.msg_controllen = 0;
	msg.msg_flags = 0;
	msg.msg_iov = &v;
	msg.msg_iovlen = 1;

	sa.sin_addr.s_addr = inet_addr("198.51.100.0");
	msg.msg_name = &sa;
	msg.msg_namelen = sizeof(sa);

	for(int i = 0; i < 16; i++)
	{
		int flags = MSG_MORE;
		if(i == 15)
			flags &= ~MSG_MORE;
		
		int st = sendmsg(sock, &msg, flags);
		if(st < 0)
		{
			perror("sendmsg");
			ASSERT_NE(st, -1);
		}

		ASSERT_EQ(st, v.iov_len);
	}

	close(sock);
}

TEST(Udp, Inet4CorkEMsgSize)
{
	int sock = socket(AF_INET, SOCK_DGRAM, 0);

	ASSERT_NE(sock, -1);

	sockaddr_in sa = {};
	sa.sin_addr.s_addr = INADDR_ANY;
	sa.sin_port = htons(1066);
	sa.sin_family = AF_INET;

	ASSERT_NE(bind(sock, (const sockaddr *) &sa, sizeof(sa)), -1);

#if 0
	int off = IP_PMTUDISC_DONT;
	ASSERT_NE(setsockopt(sock, SOL_IP, IP_MTU_DISCOVER, &off, sizeof(off)), -1);
#endif

	void *ptr = new char[std::numeric_limits<uint16_t>::max()];

	constexpr auto max_size = UINT16_MAX - 8 /* sizeof udphdr */ - sizeof(iphdr);
	iovec v;
	v.iov_base = ptr;
	v.iov_len = max_size / 16;

	msghdr msg;
	msg.msg_control = nullptr;
	msg.msg_controllen = 0;
	msg.msg_flags = 0;
	msg.msg_iov = &v;
	msg.msg_iovlen = 1;

	sa.sin_addr.s_addr = inet_addr("198.51.100.0");
	msg.msg_name = &sa;
	msg.msg_namelen = sizeof(sa);

	for(int i = 0; i < 16; i++)
	{
		int flags = MSG_MORE;
		
		int st = sendmsg(sock, &msg, flags);
		if(st < 0)
		{
			perror("sendmsg");
			ASSERT_NE(st, -1);
		}

		ASSERT_EQ(st, v.iov_len);
	}

	ASSERT_EQ(sendmsg(sock, &msg, 0), -1);
	ASSERT_EQ(errno, EMSGSIZE);

	close(sock);
}
