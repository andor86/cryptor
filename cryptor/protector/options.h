#pragma once
struct Options
{
	//�������������� �������� - ����� �������� ����
	//������������ ������������ ����
	bool force_mode;
	//�������� �� DOS-���������
	bool strip_dos_headers;
	bool anti_debug;
	//���������� ������
	int crypt_mode;
	//�������������� �� ���������� ������������ ��������
	bool rebuild_load_config;
	//�������������� �� �������
	bool repack_resources = true;
	//�������� ������������ ����� ��������
	unsigned long file_alignment = 512;//512
};
