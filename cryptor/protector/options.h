#pragma once
struct Options
{
	//�������������� �������� - ����� �������� ����
	//������������ ������������ ����
	bool force_mode;
	//�������� �� DOS-���������
	bool strip_dos_headers;
	bool rc5;
	bool anti_debug;
	//���������� ������
	bool crypt;
	//�������������� �� ���������� ������������ ��������
	bool rebuild_load_config;
	//�������������� �� �������
	bool repack_resources = true;
	//�������� ������������ ����� ��������
	unsigned long file_alignment = 512;//512

};
