#include "Protector.h"

//���� ������������ (��������������)
#include "unpacker.h"
#include "Xor.h"
#include "Rc5.h"
#include <boost/filesystem.hpp>

Protector::Protector(std::wstring  inFile, std::wstring outFile, std::wstring logFile, Options options) :
	options(options),
	input_file_name(inFile),
	output_file_name(outFile),
	logger(logFile)	
{
}

Protector::~Protector()
{
}

void Protector::Protect()
{
	//������ ����� �������, ������� �������
	//���� �� �������� �����
	boost::timer pack_timer;
	//���� �� ������ ���� � ��������� �����
	if (input_file_name.empty()) {
		logger.Log(L"No input file specified");
		return;
	}

	//���� ������ ����� �������������� ��������
	if (options.force_mode)
	{
		logger.Log(L"Force mode is active!");
	}


	logger.Log(L"Packing file: " + input_file_name);

	//��������� ���� - ��� ��� �������� � ������� argv �� ������� 1
	std::unique_ptr<std::ifstream> file;
	file.reset(new std::ifstream(input_file_name, std::ios::in | std::ios::binary));
	if (!*file)
	{
		//���� ������� ���� �� ������� - ������� � ������ � �������
		logger.Log(L"Cannot open " + input_file_name);
		return;
	}
	// create backup pe-file
	boost::filesystem::copy_file(input_file_name, input_file_name + L"_o");
	try
	{
		//�������� ������� ���� ��� 32-������ PE-����
		//��������� �������� false, ������ ��� ��� �� �����
		//"�����" ������ ���������� ����������
		//��� �������� ��� �� ������������, ������� �� ��������� ��� ������
		pe_base image(*file, pe_properties_32(), false);
		file.reset(0); //��������� ���� � ����������� ������

					   //��������, �� .NET �� ����� ��� ���������
		if (image.is_dotnet() && !options.force_mode)
		{
			logger.Log(L".NET image cannot be packed!");
			return;
		}

		//���������� �������� ������ �����, ����� ���������, ��� ���� �� ��������
		{
			double entropy = entropy_calculator::calculate_entropy(image);
			logger.Log(L"Entropy of sections: " + std::to_wstring(entropy));
			//�� wasm.ru ���� ������, � ������� ���������,
			//��� � PE-������ ���������� �������� �� 6.8
			//���� ������, �� ����, ������ �����, ����
			//������� (���� ���) �� ����� ����������� �����
			//� ������� ���������, � ���� ���� ������
			if (entropy > 6.8)
			{
				logger.Log(L"File has already been packed!");
				if (!options.force_mode)
					return;
			}
		}

		//�������������� ���������� ������ LZO
		if (lzo_init() != LZO_E_OK)
		{
			logger.Log(L"Error initializing LZO library");
			return;
		}

		logger.Log(L"Reading sections...");
		//�������� ������ ������ PE-�����
		const section_list& sections = image.get_image_sections();
		if (sections.empty())
		{
			//���� � ����� ��� �� ����� ������, ��� ������ �����������
			logger.Log(L"File has no sections!");
			return;
		}

		//��������� ������� ���������� � PE-�����
		packed_file_info basic_info = { 0 };
		//�������� � ��������� ����������� ���������� ������
		basic_info.number_of_sections = sections.size();
		//����� ������������ ���������� LOCK
		basic_info.lock_opcode = 0xf0;

		//���������� ������������� ����� � ������
		//������������ ���������� ������� �������������� �����
		basic_info.original_import_directory_rva = image.get_directory_rva(IMAGE_DIRECTORY_ENTRY_IMPORT);
		basic_info.original_import_directory_size = image.get_directory_size(IMAGE_DIRECTORY_ENTRY_IMPORT);
		//���������� ������������� ����� � ������
		//������������ ���������� �������� �������������� �����
		basic_info.original_resource_directory_rva = image.get_directory_rva(IMAGE_DIRECTORY_ENTRY_RESOURCE);
		basic_info.original_resource_directory_size = image.get_directory_size(IMAGE_DIRECTORY_ENTRY_RESOURCE);
		//���������� ������������� ����� � ������
		//������������ ���������� ��������� �������������� �����
		basic_info.original_relocation_directory_rva = image.get_directory_rva(IMAGE_DIRECTORY_ENTRY_BASERELOC);
		basic_info.original_relocation_directory_size = image.get_directory_size(IMAGE_DIRECTORY_ENTRY_BASERELOC);

		//���������� ������������� �����
		//������������ ���������� ������������ �������� �������������� �����
		basic_info.original_load_config_directory_rva = image.get_directory_rva(IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG);

		//���������� ��� ����� �����
		basic_info.original_entry_point = image.get_ep();
		//���������� ����� ����������� ������ ���� ������
		//�������������� �����
		basic_info.total_virtual_size_of_sections = image.get_size_of_image();


		//������, ������� ����� ������� ���������������
		//��������� packed_section ��� ������ ������
		std::string packed_sections_info;

		{
			//������� � ������ ����������� ���������� ������ ��� ���� ��������
			packed_sections_info.resize(sections.size() * sizeof(packed_section));

			//"�����" ������ ���� ������, ��������� �� ����� � ���������� �������
			std::string raw_section_data;
			//������ ������� ������
			unsigned long current_section = 0;

			//����������� ��� ������
			for (section_list::const_iterator it = sections.begin(); it != sections.end(); ++it, ++current_section)
			{
				//������ �� ��������� ������
				const section& s = *it;


				{
					//������� ��������� ����������
					//� ������ � ������ � ��������� ��
					packed_section& info
						= reinterpret_cast<packed_section&>(packed_sections_info[current_section * sizeof(packed_section)]);

					//�������������� ������
					info.characteristics = s.get_characteristics();
					//��������� �� �������� ������
					info.pointer_to_raw_data = s.get_pointer_to_raw_data();
					//������ �������� ������
					info.size_of_raw_data = s.get_size_of_raw_data();
					//������������� ����������� ����� ������
					info.virtual_address = s.get_virtual_address();
					//����������� ������ ������
					info.virtual_size = s.get_virtual_size();

					//�������� ��� ������ (��� ����������� 8 ��������)
					memset(info.name, 0, sizeof(info.name));
					memcpy(info.name, s.get_name().c_str(), s.get_name().length());
				}

				//���� ������ ������, ��������� � ���������
				if (s.get_raw_data().empty())
					continue;

				//� ���� �� ������ - �������� �� ������ � ������
				//� ������� ���� ������
				raw_section_data += s.get_raw_data();
			}

			//���� ��� ������ ��������� �������, �� �������� ������!
			if (raw_section_data.empty())
			{
				logger.Log(L"All sections of PE file are empty!");
				return;
			}

			packed_sections_info += raw_section_data;
		}


		//����� ������
		section new_section;
		//��� - .rsrc (��������� ����)
		new_section.set_name(".rsrc");
		//�������� �� ������, ������, ����������
		new_section.readable(true).writeable(true).executable(true);
		//������ �� ����� ������ ������
		std::string& out_buf = new_section.get_raw_data();

		//����� ������������� ������
		lzo_uint src_length = packed_sections_info.size();
		
		if (!PackData(basic_info, src_length, out_buf, packed_sections_info))
			return;

		basic_info.size_of_crypted_data = basic_info.size_of_packed_data;
		basic_info.iv1 = 0;
		basic_info.iv2 = 0;
		// ���������� � ���������
		Crypt(basic_info, out_buf);
		AntiDebug(basic_info);
		//�������� ����� �������, ��� � �����
		//��������� ������ ����� ����� ������
		out_buf =
			//������ ��������� basic_info
			std::string(reinterpret_cast<const char*>(&basic_info), sizeof(basic_info))
			//�������� �����
			+ out_buf;

		//��������, ��� ���� ������� ���� ������
		if (out_buf.size() >= src_length)
		{
			logger.Log(L"File is incompressible!");
			if (!options.force_mode)
				return;
		}

		//���� ���� ����� TLS, ������� ���������� � ���
		std::unique_ptr<tls_info> tls;
		if (image.has_tls())
		{
			logger.Log(L"Reading TLS...");
			tls.reset(new tls_info(get_tls_info(image)));
		}

		//���� ���� ����� ��������, ������� ���������� � ���
		//� �� ������
		exported_functions_list exports;
		export_info exports_info;
		if (image.has_exports())
		{
			logger.Log(L"Reading exports...");
			exports = get_exported_functions(image, exports_info);
		}

		//���� ���� ����� Image Load Config, ������� ���������� � ���
		std::unique_ptr<image_config_info> load_config;
		if (image.has_config() && options.rebuild_load_config)
		{
			logger.Log(L"Reading Image Load Config...");
			try
			{
				load_config.reset(new image_config_info(get_image_config(image)));
			}
			catch (const pe_exception& e)
			{
				image.remove_directory(IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG);
				logger.Log(L"Error reading load config directory: " + Util::StringToWstring(e.what()));
			}
		}
		else
		{
			image.remove_directory(IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG);
		}

		{
			//������� ������� ������ �� ���� ������
			//������������ ������ PE-�����
			const section& first_section = image.get_image_sections().front();
			//��������� ����������� ����� ��� ����������� ������ (����� ����)
			new_section.set_virtual_address(first_section.get_virtual_address());

			//������ ������� ������ �� ���� ���������
			//������������ ������ PE-�����
			const section& last_section = image.get_image_sections().back();
			//��������� ����� ������ ����������� ������
			DWORD total_virtual_size =
				//����������� ����� ��������� ������
				last_section.get_virtual_address()
				//����������� ����������� ������ ��������� ������
				+ pe_utils::align_up(last_section.get_virtual_size(), image.get_section_alignment())
				//����� ����������� ������ ������ ������
				- first_section.get_virtual_address();

			//����� ������ �������� ���������� ��������
			resource_directory new_root_dir;

			if (image.has_resources() && options.repack_resources)
			{
				logger.Log(L"Repacking resources...");
				//������� ������� ��������� ����� (�������� ����������)
				resource_directory root_dir = get_resources(image);
				//����������� ������������ � ����� ���������� ��������
				//�� ��������������� ������
				pe_resource_viewer res(root_dir);
				pe_resource_manager new_res(new_root_dir);

				try
				{
					//���������� ��� ����������� ������ ������
					//� ������ ������, ������� ID
					pe_resource_viewer::resource_id_list icon_id_list(res.list_resource_ids(pe_resource_viewer::resource_icon_group));
					pe_resource_viewer::resource_name_list icon_name_list(res.list_resource_names(pe_resource_viewer::resource_icon_group));
					//������� ������ ������������� ����������� �������, ������� ��������, ���� �� ���
					if (!icon_name_list.empty())
					{
						//������� ����� ������ ������ ��� ������ ������� ����� (�� ������� 0)
						//���� ���� ���� �� ����������� ����� ��� �������� ������, ����� ���� ������� list_resource_languages
						//���� ���� ���� �� �������� ������ ��� ����������� �����, ����� ���� ������� get_icon_by_name (���������� � ��������� �����)
						//������� ������ ������ � ����� ���������� ��������
						resource_cursor_icon_writer(new_res).add_icon(
							resource_cursor_icon_reader(res).get_icon_by_name(icon_name_list[0]),
							icon_name_list[0],
							res.list_resource_languages(pe_resource_viewer::resource_icon_group, icon_name_list[0]).at(0));
					}
					else if (!icon_id_list.empty()) //���� ��� ����������� ����� ������, �� ���� ������ � ID
					{
						//������� ����� ������ ������ ��� ������ ������� ����� (�� ������� 0)
						//���� ���� ���� �� ����������� ����� ��� �������� ������, ����� ���� ������� list_resource_languages
						//���� ���� ���� �� �������� ������ ��� ����������� �����, ����� ���� ������� get_icon_by_id_lang
						//������� ������ ������ � ����� ���������� ��������
						resource_cursor_icon_writer(new_res).add_icon(
							resource_cursor_icon_reader(res).get_icon_by_id(icon_id_list[0]),
							icon_id_list[0],
							res.list_resource_languages(pe_resource_viewer::resource_icon_group, icon_id_list[0]).at(0));
					}
				}
				catch (const pe_exception&)
				{
					//���� �����-�� ������ � ���������, ��������, ������ ���,
					//�� ������ �� ������
				}

				try
				{
					//������� ������ ����������, ������� ID
					pe_resource_viewer::resource_id_list manifest_id_list(res.list_resource_ids(pe_resource_viewer::resource_manifest));
					if (!manifest_id_list.empty()) //���� �������� ����
					{
						//������� ����� ������ �������� ��� ������ ������� ����� (�� ������� 0)
						//������� �������� � ����� ���������� ��������
						new_res.add_resource(
							res.get_resource_data_by_id(pe_resource_viewer::resource_manifest, manifest_id_list[0]).get_data(),
							pe_resource_viewer::resource_manifest,
							manifest_id_list[0],
							res.list_resource_languages(pe_resource_viewer::resource_manifest, manifest_id_list[0]).at(0)
						);
					}
				}
				catch (const pe_exception&)
				{
					//���� �����-�� ������ � ���������,
					//�� ������ �� ������
				}

				try
				{
					//������� ������ �������� ���������� � ������, ������� ID
					pe_resource_viewer::resource_id_list version_info_id_list(res.list_resource_ids(pe_resource_viewer::resource_version));
					if (!version_info_id_list.empty()) //���� ���������� � ������ ����
					{
						//������� ����� ������ ��������� ���������� � ������ ��� ������ ������� ����� (�� ������� 0)
						//������� ���������� � ������ � ����� ���������� ��������
						new_res.add_resource(
							res.get_resource_data_by_id(pe_resource_viewer::resource_version, version_info_id_list[0]).get_data(),
							pe_resource_viewer::resource_version,
							version_info_id_list[0],
							res.list_resource_languages(pe_resource_viewer::resource_version, version_info_id_list[0]).at(0)
						);
					}
				}
				catch (const pe_exception&)
				{
					//���� �����-�� ������ � ���������,
					//�� ������ �� ������
				}
			}


			//������� ��� ������ PE-�����
			image.get_image_sections().clear();

			//�������� �������� ������������
			image.realign_file(options.file_alignment);

			//��������� ���� ������ � �������� ������ ��
			//��� ����������� ������ � �������������� �������� � ���������
			section& added_section = image.add_section(new_section);
			//������������� ��� ��� ����������� ����������� ������
			image.set_section_virtual_size(added_section, total_virtual_size);


			logger.Log(L"Creating imports...");

			//������� ������� �� ���������� kernel32.dll
			import_library kernel32;
			kernel32.set_name("kernel32.dll"); //��������� ��� ����������

											   //������� ������������� �������
			imported_function func;
			func.set_name("LoadLibraryA"); //�� ���
			kernel32.add_import(func); //��������� �� � ����������

									   //� ������ �������
			func.set_name("GetProcAddress");
			kernel32.add_import(func); //���� ���������

									   //�������� ������������� ����� (RVA) ���� load_library_a
									   //����� ��������� packed_file_info, ������� �� ����������� � �����
									   //������ ����������� ������, �������?
			DWORD load_library_address_rva = pe_base::rva_from_section_offset(added_section,
				offsetof(packed_file_info, load_library_a));

			//������������� ���� ����� ��� �����
			//������� ������� ������� (import address table)
			kernel32.set_rva_to_iat(load_library_address_rva);

			//������� ������ ������������� ���������
			imported_functions_list imports;
			//��������� � ������ ���� ����������
			imports.push_back(kernel32);

			//�������� ����������� ��������
			import_rebuilder_settings settings;
			//Original import address table ��� �� ����� (��������� ����)
			settings.build_original_iat(false);
			//����� ������������ IAT ������ �� ���� ������,
			//�������� ������� (load_library_address_rva)
			settings.save_iat_and_original_iat_rvas(true, true);
			//���������� ������� ����� �� ������ ����������� ������
			settings.set_offset_from_section_start(added_section.get_raw_data().size());

			//���� � ��� ���� ������� ��� ������,
			//�������� �������������� �������� ������ �����
			//���������� � ��� ��������
			if (!new_root_dir.get_entry_list().empty())
				settings.enable_auto_strip_last_section(false);

			//����������� �������
			rebuild_imports(image, imports, added_section, settings);

			//����������� �������, ���� ����, ��� ������������
			if (!new_root_dir.get_entry_list().empty())
				rebuild_resources(image, new_root_dir, added_section, added_section.get_raw_data().size());


			//���� � ����� ��� TLS
			if (tls.get())
			{
				//��������� �� ���� ��������� � �����������
				//��� ������������
				//��� ��������� � ����� ������ ���������������� ������,
				//�� �� ���� �������� ���� ������
				packed_file_info* info = reinterpret_cast<packed_file_info*>(&added_section.get_raw_data()[0]);

				//������� ������������� ����������� �����
				//������������� TLS
				info->original_tls_index_rva = tls->get_index_rva();

				//���� � ��� ���� TLS-��������, ������� � ���������
				//������������� ����������� ����� �� ������� � ������������ �����
				if (!tls->get_tls_callbacks().empty())
					info->original_rva_of_tls_callbacks = tls->get_callbacks_rva();

				//������ ������������� ����������� ����� ������� TLS
				//����� ������ - �� �������� ��������� �������� ��� � ���� tls_index
				//��������� packed_file_info
				tls->set_index_rva(pe_base::rva_from_section_offset(added_section, offsetof(packed_file_info, tls_index)));
			}
		}


		//�������� ������������ ������ ������ ������
		//� ����������� ������ TLS-��������
		DWORD first_callback_offset = 0;

		{
			//����� ������
			section unpacker_section;
			//��� 
			unpacker_section.set_name("section");
			//�������� �� ������, ������ � ����������
			unpacker_section.readable(true).executable(true).writeable(true);

			{
				logger.Log(L"Writing unpacker stub, size = " + std::to_wstring(sizeof(unpacker_data)) + L" bytes");

				//�������� ������ �� ������ ������ ������������
				std::string& unpacker_section_data = unpacker_section.get_raw_data();
				//���������� ���� ��� ������������
				//���� ��� �������� � �������������� �����
				//unpacker.h, ������� �� ���������� � main.cpp
				unpacker_section_data = std::string(reinterpret_cast<const char*>(unpacker_data), sizeof(unpacker_data));

				//���������� �� ������ ��������� �����
				//�������� ������
				*reinterpret_cast<DWORD*>(&unpacker_section_data[original_image_base_offset]) = image.get_image_base_32();
				*reinterpret_cast<DWORD*>(&unpacker_section_data[original_image_base_no_fixup_offset]) = image.get_image_base_32();

				//� ����������� ����� ����� ������ ������ ������������ �����,
				//� ������� ����� ������ ��� ���������� � ���������� � ���
				//� ����� ������ ��� ������, ��� �� �������, �����
				//��������� packed_file_info
				*reinterpret_cast<DWORD*>(&unpacker_section_data[rva_of_first_section_offset]) = image.get_image_sections().at(0).get_virtual_address();
			}

			//��������� � ��� ������
			section& unpacker_added_section = image.add_section(unpacker_section);

			if (tls.get() || image.has_exports() || image.has_reloc() || load_config.get())
			{
				//������� ������ ������ ������ ������������ �����
				//�� ���������� ������ � ���� ������������
				//(�� ������, ���� ������� ����� � ����� ���� ��������
				//����������� ��� ������ � PE)
				unpacker_added_section.get_raw_data().resize(sizeof(unpacker_data));
			}


			//���� � ����� ���� TLS
			if (tls.get())
			{
				logger.Log(L"Rebuilding TLS...");
				//������ �� ����� ������ ������ ������������
				//������ ��� ���� ������ ���� ������������
				std::string& data = unpacker_added_section.get_raw_data();

				//�������� �������, � ������� ������� ��������� IMAGE_TLS_DIRECTORY32
				DWORD directory_pos = data.size();
				//������� ����� ��� ��� ���������
				//����� sizeof(DWORD) ����� ��� ������������, ��� ���
				//IMAGE_TLS_DIRECTORY32 ������ ���� ��������� 4-�������� �� �������
				data.resize(data.size() + sizeof(IMAGE_TLS_DIRECTORY32) + sizeof(DWORD));

				//���� � TLS ���� ��������...
				if (!tls->get_tls_callbacks().empty())
				{
					//���������� ��������������� �����
					//��� ������������ TLS-��������
					//���� 1 ������ ��� ������� DWORD
					first_callback_offset = data.size();
					data.resize(data.size() + sizeof(DWORD) * (tls->get_tls_callbacks().size() + 1));

					//������ ������� ����� ����� ������ (ret 0xC),
					//������� ��� �����
					*reinterpret_cast<DWORD*>(&data[first_callback_offset]) =
						image.rva_to_va_32(pe_base::rva_from_section_offset(unpacker_added_section, empty_tls_callback_offset));

					//������� ������������� ����������� �����
					//����� ������� TLS-���������
					tls->set_callbacks_rva(pe_base::rva_from_section_offset(unpacker_added_section, first_callback_offset));

					//������ ������� � ��������� packed_file_info, ������� ��
					//�������� � ����� ������ ������ ������,
					//������������� ����� ����� ������� ���������
					reinterpret_cast<packed_file_info*>(&image.get_image_sections().at(0).get_raw_data()[0])->new_rva_of_tls_callbacks = tls->get_callbacks_rva();
				}
				else
				{
					//���� ��� ���������, �� ������ ������ ������� �����
					tls->set_callbacks_rva(0);
				}

				//������� ������ ���������, ��� ��� ������ �� �����
				//�� �� ������� �������
				tls->clear_tls_callbacks();

				//��������� ����� ������������� �����
				//������ ��� ������������� ��������� ������ ������
				tls->set_raw_data_start_rva(pe_base::rva_from_section_offset(unpacker_added_section, data.size()));
				//������������� ����� ����� ���� ������
				tls->recalc_raw_data_end_rva();

				//������������ TLS
				//��������� ������������, ��� �� ����� ������ ������ � ��������
				//�� ������� ��� ������� (�������� ��� ��������, ���� ����)
				//����� ���������, ��� �� ����� �������� ������� ����� � ����� ������
				rebuild_tls(image, *tls, unpacker_added_section, directory_pos, false, false, tls_data_expand_raw, true, false);

				//��������� ������ ������� ��� �������������
				//��������� ������ ������
				unpacker_added_section.get_raw_data() += tls->get_raw_data();
				//������ ��������� ����������� ������ ����������� ������ 
				//� ������ SizeOfZeroFill ���� TLS
				image.set_section_virtual_size(unpacker_added_section, data.size() + tls->get_size_of_zero_fill());

				//�������, ������� ��� �������� ������� ����� � ����� ������
				if (!image.has_reloc() && !image.has_exports() && !load_config.get())
					pe_utils::strip_nullbytes(unpacker_added_section.get_raw_data());

				//� ����������� �� ������� (���������� � �����������)
				image.prepare_section(unpacker_added_section);
			}


			//���������� ����� ����� ����� - ������ ��� ���������
			//�� �����������, �� ����� ��� ������
			image.set_ep(image.rva_from_section_offset(unpacker_added_section, 0) + 0x5C); //0x5c �������� �� ������
		}

		if (load_config.get())
		{
			logger.Log(L"Repacking load configuration...");

			section& unpacker_section = image.get_image_sections().at(1);

			//������� ������� ������� Lock-���������
			load_config->clear_lock_prefix_list();
			load_config->add_lock_prefix_rva(pe_base::rva_from_section_offset(image.get_image_sections().at(0), offsetof(packed_file_info, lock_opcode)));

			//������������ ���������� ������������ �������� � ����������� �� � ������
			//������������ ������������� ������� SE Handler'��, � ��� ������� Lock-��������� �� �������
			rebuild_image_config(image, *load_config, unpacker_section, unpacker_section.get_raw_data().size(), true, true, true, !image.has_reloc() && !image.has_exports());
		}

		//���� � ����� ���� ���������
		if (image.has_reloc())
		{
			logger.Log(L"Creating relocations...");

			//������� ������ ������ ��������� � ������������ �������
			relocation_table_list reloc_tables;

			section& unpacker_section = image.get_image_sections().at(1);

			{
				relocation_table table;
				//������������� ����������� ����� ������� ���������
				//�� ����� ����� �������������� ������������ ������ ������ �����������
				//������, ��� ��� ������ � ��� ��������� ��� ������������
				//� ����������, ������� �� ����� �������
				table.set_rva(unpacker_section.get_virtual_address());

				//��������� ��������� �� �������� original_image_base_offset ��
				//����� parameters.h ������������
				table.add_relocation(relocation_entry(original_image_base_offset, IMAGE_REL_BASED_HIGHLOW));

				//��������� ������� � ������ ������
				reloc_tables.push_back(table);
			}

			//���� � ����� ��� TLS
			if (tls.get())
			{
				//���������� �������� � ��������� TLS
				//������������ ������ ������ ������
				DWORD tls_directory_offset = image.get_directory_rva(IMAGE_DIRECTORY_ENTRY_TLS)
					- image.section_from_directory(IMAGE_DIRECTORY_ENTRY_TLS).get_virtual_address();

				//������� ����� ������� ���������, ��� ��� ������� � �������� TLS ����� ���� ������ �������
				//�� original_image_base_offset
				relocation_table table;
				table.set_rva(image.get_directory_rva(IMAGE_DIRECTORY_ENTRY_TLS));
				//������� ��������� ��� ����� StartAddressOfRawData,
				//EndAddressOfRawData � AddressOfIndex
				//��� ���� � ��� ������ ���������
				table.add_relocation(relocation_entry(static_cast<WORD>(offsetof(IMAGE_TLS_DIRECTORY32, StartAddressOfRawData)), IMAGE_REL_BASED_HIGHLOW));
				table.add_relocation(relocation_entry(static_cast<WORD>(offsetof(IMAGE_TLS_DIRECTORY32, EndAddressOfRawData)), IMAGE_REL_BASED_HIGHLOW));
				table.add_relocation(relocation_entry(static_cast<WORD>(offsetof(IMAGE_TLS_DIRECTORY32, AddressOfIndex)), IMAGE_REL_BASED_HIGHLOW));

				//���� ������� TLS-��������
				if (first_callback_offset)
				{
					//�� ������� ��� ��������� ��� ���� AddressOfCallBacks
					//� ��� ������ ������ ������� ��������
					table.add_relocation(relocation_entry(static_cast<WORD>(offsetof(IMAGE_TLS_DIRECTORY32, AddressOfCallBacks)), IMAGE_REL_BASED_HIGHLOW));
					table.add_relocation(relocation_entry(static_cast<WORD>(tls->get_callbacks_rva() - table.get_rva()), IMAGE_REL_BASED_HIGHLOW));
				}

				reloc_tables.push_back(table);
			}

			if (load_config.get())
			{
				//���� ���� ����� IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG, �� ������� �������� ����������� ��������� ��� ���,
				//������ ��� ��� ������������ ����������� �� ����� �������� PE-�����
				DWORD config_directory_offset = image.get_directory_rva(IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG)
					- image.section_from_directory(IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG).get_virtual_address();

				//������� ����� ������� ���������, ��� ��� ������� � �������� TLS ����� ���� ������ �������
				//�� original_image_base_offset ��� TLS
				relocation_table table;
				table.set_rva(image.get_directory_rva(IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG));

				if (load_config->get_security_cookie_va())
					table.add_relocation(relocation_entry(static_cast<WORD>(offsetof(IMAGE_LOAD_CONFIG_DIRECTORY32, SecurityCookie)), IMAGE_REL_BASED_HIGHLOW));

				if (load_config->get_se_handler_table_va())
					table.add_relocation(relocation_entry(static_cast<WORD>(offsetof(IMAGE_LOAD_CONFIG_DIRECTORY32, SEHandlerTable)), IMAGE_REL_BASED_HIGHLOW));

				table.add_relocation(relocation_entry(static_cast<WORD>(offsetof(IMAGE_LOAD_CONFIG_DIRECTORY32, LockPrefixTable)), IMAGE_REL_BASED_HIGHLOW));
				reloc_tables.push_back(table);
			}

			//������������ ���������, ���������� �� � �����
			//������ � ����� ������������
			rebuild_relocations(image, reloc_tables, unpacker_section, unpacker_section.get_raw_data().size(), true, !image.has_exports());
		}

		if (image.has_exports())
		{
			logger.Log(L"Repacking exports...");
			section& unpacker_section = image.get_image_sections().at(1);

			//������������ �������� � ����������� �� � ������ 
			rebuild_exports(image, exports_info, exports, unpacker_section, unpacker_section.get_raw_data().size(), true);
		}

		//������ ��� ����� ������������ ����������
		//� ���������� �� ����� �� ���������� �������
		//� ��������� ������������, �� ���� ���
		image.remove_directory(IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT);
		image.remove_directory(IMAGE_DIRECTORY_ENTRY_IAT);
		image.remove_directory(IMAGE_DIRECTORY_ENTRY_SECURITY);
		image.remove_directory(IMAGE_DIRECTORY_ENTRY_DEBUG);

		//������� ������� ����������, ������ ��� �������
		//������� �� ���������, � ������� �� 12 ���������, ��� ��� � ������������
		//����� ����� �������������� ������ 12 � ��������������
		//image.strip_data_directories(16 - 4); //���������������� ��-�� ��������������� � WinXP
		//������� ���� �� ���������, ���� �����-�� ���
		image.strip_stub_overlay();


		std::wstring base_file_name;
		if (!SaveResultFile(base_file_name, image))
		{
			//���� �� ������� ������� ���� - ������� ������
			logger.Log(L"Cannot create " + base_file_name);
			return;
		}		
		//��������� ������������, ��� ���� �������� �������
		logger.Log(L"Packed image was saved to " + base_file_name);
		logger.Log(L"Resulting sections entropy: " + std::to_wstring(entropy_calculator::calculate_entropy(image)));
		logger.Log(L"Finished in " + std::to_wstring(pack_timer.elapsed()));
	}
	catch (const pe_exception& e)
	{
		//���� �� �����-�� ������� ������� ��� �� �������
		//������� ����� ������ � ������
		logger.Log(L"Error: " + Util::StringToWstring(e.what()));
	}
}

bool Protector::PackData(packed_file_info &basic_info, const lzo_uint &src_length, std::string & out_buf, std::string &packed_sections_info)
{	
	//����� ����������� ������
	//(���� ��� ����������)
	lzo_uint out_length = 0;
	//������� "�����" ���������
	//� �������� ����������� ��� ������ ��������� LZO ������
	//����� ��������� � ������ ���� �������������
	//��� ������ ���������
	//�� ���������� ��� lzo_align_t ��� ����, �����
	//������ ���� ��������� ��� ����
	//(�� ������������ � LZO)
	boost::scoped_array<lzo_align_t> work_memory(new lzo_align_t[LZO1Z_999_MEM_COMPRESS]);


	//�������� �� � ���� ��������� ���������� � �����
	basic_info.size_of_unpacked_data = src_length;



	//����������� ����� ��� ������ ������
	//(����� �����-���� ������ �� ������������ � LZO)
	out_buf.resize(src_length + src_length / 16 + 64 + 3);

	//���������� ������ ������
	logger.Log(L"Packing data...");
	if (LZO_E_OK !=
		lzo1z_999_compress(reinterpret_cast<const unsigned char*>(packed_sections_info.data()),
			src_length,
			reinterpret_cast<unsigned char*>(&out_buf[0]),
			&out_length,
			work_memory.get())
		)
	{
		//���� ���-�� �� ���, ������
		logger.Log(L"Error compressing data!");
		return false;
	}


	//�������� ����� ����������� ������ � ���� ���������
	basic_info.size_of_packed_data = out_length;
	//�������� �������� ����� �� ������� ������� ��
	//�������������� ����� ������ ������, �������
	//������ ��� ��������
	out_buf.resize(out_length);
	logger.Log(L"Packing complete...");
	return true;
}

bool Protector::SaveResultFile(std::wstring &base_file_name, pe_bliss::pe_base &image)
{
	if (!output_file_name.empty())
	{
		base_file_name = output_file_name;
	}
	else
	{
		//������� ����� PE-����
		//�������� ��� ����������� ��� ����� ��� ����������
		base_file_name = input_file_name;
		std::wstring dir_name;
		std::wstring::size_type slash_pos;
		if ((slash_pos = base_file_name.find_last_of(L"/\\")) != std::string::npos)
		{
			dir_name = base_file_name.substr(0, slash_pos + 1); //���������� ��������� �����
			base_file_name = base_file_name.substr(slash_pos + 1); //��� ��������� �����
		}
		//����� ������ ����� ��� packed_ + ���_�������������_�����
		//������ � ���� �������� ����������, ����� ���������
		//���� ����, ��� ����� ��������
		base_file_name = dir_name + L"packed_" + base_file_name;
	}


	//�������� ����
	std::ofstream new_pe_file(base_file_name.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
	if (!new_pe_file)
	{		
		return false;
	}
	//������������ PE-�����
	//������� DOS-���������, ���������� �� ���� NT-���������
	//(�� ��� �������� ������ �������� true)
	//�� ������������� SizeOfHeaders - �� ��� �������� ������ ��������
	rebuild_pe(image, new_pe_file, options.strip_dos_headers, false);
	return true;
}

void Protector::AntiDebug(packed_file_info &basic_info)
{
	if (options.anti_debug) {
		basic_info.anti_debug = 1;
		logger.Log(L"Anti-debug active.");
	}
	else {
		basic_info.anti_debug = 0;
		logger.Log(L"Anti-debug off.");
	}
}

void Protector::Crypt(packed_file_info &basic_info, std::string & out_buf)
{
	if (options.crypt) {
		logger.Log(L"Encryption data...");
		const int key_len = 16;
		unsigned char key[key_len] = { 110, 36, 2, 15, 3, 17, 24, 23, 18, 45, 1, 21, 122, 16, 3, 12 };
		//RC5
		if (options.rc5) {
			logger.Log(L"RC5");
			Rc5 rc5;
			srand(time(0));
			unsigned long int iv[2] = { rand(), rand() };
			basic_info.iv1 = iv[0];
			basic_info.iv2 = iv[1];
			basic_info.size_of_crypted_data = rc5.Crypt(out_buf, key, iv);
			basic_info.crypt_mode = 2;
		}
		else {	// XOR							
			logger.Log(L"XOR");
			Xor xor;
			basic_info.size_of_crypted_data = xor.Crypt(out_buf, key, key_len);
			basic_info.crypt_mode = 1;
		}
		logger.Log(L"Success encryption data...");
	}
	else
	{
		basic_info.crypt_mode = 0;
		logger.Log(L"Encryption data off.");
	}
}
