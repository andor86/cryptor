﻿#include "protector.h"

std::string to_utf8(const wchar_t* buffer, int len)
{
	int nChars = ::WideCharToMultiByte(
		CP_UTF8,
		0,
		buffer,
		len,
		NULL,
		0,
		NULL,
		NULL);
	if (nChars == 0) return "";

	std::string newbuffer;
	newbuffer.resize(nChars);
	::WideCharToMultiByte(
		CP_UTF8,
		0,
		buffer,
		len,
		const_cast< char* >(newbuffer.c_str()),
		nChars,
		NULL,
		NULL);

	return newbuffer;
}

std::string to_utf8(const std::wstring& str)
{
	return to_utf8(str.c_str(), (int)str.size());
}



int protect(WCHAR * inFile, WCHAR * outFile, WCHAR * logFile, options opt)
{	

	//Таймер будет считать, сколько времени
	//ушло на упаковку файла
	boost::timer pack_timer;

	//Принудительная упаковка - будет упакован даже
	//потенциально некорректный файл
	bool force_mode =opt.force_mode;
	//Перепаковывать ли ресурсы
	bool repack_resources=true;
	//Перепаковывать ли директорию конфигурации загрузки
	bool rebuild_load_config=opt.rebuild_load_config;
	//Обрезать ли DOS-заголовок
	bool strip_dos_headers=opt.strip_dos_headers;
	//Файловое выравнивание после упаковки
	unsigned long file_alignment=512;//512
	//Шифрование данных
	bool encrypt = opt.crypt;
	
	//Путь к исходному файлу
	std::wstring input_file_name(inFile);
	//Путь для упакованного файла
	std::wstring output_file_name(outFile);
	// Путь для лог файла
	std::wstring log_file_name(logFile);

	std::ofstream log_file;
	log_file.open(log_file_name);

	//Если не указан путь к исходному файлу
	if (input_file_name.empty()){
		log_file << "No input file specified" << std::endl;
		LogEdit(L"No input file specified\r\n");
	}
	WCHAR* backupFile = new WCHAR[MAX_PATH];
	wcscpy_s(backupFile, MAX_PATH, inFile);
	wcscat_s(backupFile, MAX_PATH, L"_o");
	CopyFile(inFile, backupFile,false);

	//Если указан режим принудительной упаковки
	if (force_mode)
	{
		log_file << "Force mode is active!" << std::endl;
		LogEdit(L"Force mode is active!\r\n");
	}


	log_file << "Packing file: " << to_utf8(input_file_name) << std::endl;
	LogEdit(L"Packing file:" + input_file_name + L"\r\n");
	//Открываем файл - его имя хранится в массиве argv по индексу 1
	std::auto_ptr<std::ifstream> file;
	file.reset(new std::ifstream(input_file_name, std::ios::in | std::ios::binary));
	if(!*file)
	{
		//Если открыть файл не удалось - сообщим и выйдем с ошибкой
		log_file << "Cannot open " << to_utf8(input_file_name) << std::endl;
		LogEdit(L"Cannot open " + input_file_name + L"\r\n");
		return -1;
	}

	try
	{
		//Пытаемся открыть файл как 32-битный PE-файл
		//Последний аргумент false, потому что нам не нужны
		//"сырые" данные отладочной информации
		//При упаковке они не используются, поэтому не загружаем эти данные
		pe_base image(*file, pe_properties_32(), false);
		file.reset(0); //Закрываем файл и освобождаем память

		//Проверим, не .NET ли образ нам подсунули
		if(image.is_dotnet() && !force_mode)
		{
			log_file << ".NET image cannot be packed!" << std::endl;
			LogEdit(L".NET image cannot be packed!\r\n");
			return -1;
		}

		//Просчитаем энтропию секций файла, чтобы убедиться, что файл не упакован
		{
			log_file << "Entropy of sections: ";
			double entropy = entropy_calculator::calculate_entropy(image);
			log_file << entropy << std::endl;
			LogEdit(L"Entropy of sections:"+std::to_wstring(entropy)+L"\r\n");
			//На wasm.ru есть статья, в которой говорится,
			//что у PE-файлов нормальная энтропия до 6.8
			//Если больше, то файл, скорее всего, сжат
			//Поэтому (пока что) не будем упаковывать файлы
			//С высокой энтропией, в этом мало смысла
			if(entropy > 6.8)
			{
				log_file << "File has already been packed!" << std::endl;
				LogEdit(L"File has already been packed!\r\n");
				if (!force_mode)
					return -1;
			}
		}

		//Инициализируем библиотеку сжатия LZO
		if(lzo_init() != LZO_E_OK)
		{
			log_file << "Error initializing LZO library" << std::endl;
			LogEdit(L"Error initializing LZO library!\r\n");
			return -1;
		}

		log_file << "Reading sections..." << std::endl;
		LogEdit(L"Reading sections...\r\n");
		//Получаем список секций PE-файла
		const section_list& sections = image.get_image_sections();
		if (sections.empty())
		{
			//Если у файла нет ни одной секции, нам нечего упаковывать
			log_file << "File has no sections!" << std::endl;
			LogEdit(L"File has no sections!\r\n");
			return -1;
		}

		//Структура базовой информации о PE-файле
		packed_file_info basic_info = {0};
		//Получаем и сохраняем изначальное количество секций
		basic_info.number_of_sections = sections.size();
		//Опкод ассемблерной инструкции LOCK
		basic_info.lock_opcode = 0xf0;

		//Запоминаем относительный адрес и размер
		//оригинальной директории импорта упаковываемого файла
		basic_info.original_import_directory_rva = image.get_directory_rva(IMAGE_DIRECTORY_ENTRY_IMPORT);
		basic_info.original_import_directory_size = image.get_directory_size(IMAGE_DIRECTORY_ENTRY_IMPORT);
		//Запоминаем относительный адрес и размер
		//оригинальной директории ресурсов упаковываемого файла
		basic_info.original_resource_directory_rva = image.get_directory_rva(IMAGE_DIRECTORY_ENTRY_RESOURCE);
		basic_info.original_resource_directory_size = image.get_directory_size(IMAGE_DIRECTORY_ENTRY_RESOURCE);
		//Запоминаем относительный адрес и размер
		//оригинальной директории релокаций упаковываемого файла
		basic_info.original_relocation_directory_rva = image.get_directory_rva(IMAGE_DIRECTORY_ENTRY_BASERELOC);
		basic_info.original_relocation_directory_size = image.get_directory_size(IMAGE_DIRECTORY_ENTRY_BASERELOC);

		//Запоминаем относительный адрес
		//оригинальной директории конфигурации загрузки упаковываемого файла
		basic_info.original_load_config_directory_rva = image.get_directory_rva(IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG);

		//Запоминаем его точку входа
		basic_info.original_entry_point = image.get_ep();
		//Запоминаем общий виртуальный размер всех секций
		//упаковываемого файла
		basic_info.total_virtual_size_of_sections = image.get_size_of_image();


		//Строка, которая будет хранить последовательно
		//структуры packed_section для каждой секции
		std::string packed_sections_info;

		{
			//Выделим в строке необходимое количество памяти для этих стркуткр
			packed_sections_info.resize(sections.size() * sizeof(packed_section));

			//"Сырые" данные всех секций, считанные из файла и слепленные воедино
			std::string raw_section_data;
			//Индекс текущей секции
			unsigned long current_section = 0;

			//Перечисляем все секции
			for(section_list::const_iterator it = sections.begin(); it != sections.end(); ++it, ++current_section)
			{
				//Ссылка на очередную секцию
				const section& s = *it;


				{
					//Создаем структуру информации
					//о секции в строке и заполняем ее
					packed_section& info
						= reinterpret_cast<packed_section&>(packed_sections_info[current_section * sizeof(packed_section)]);

					//Характеристики секции
					info.characteristics = s.get_characteristics();
					//Указатель на файловые данные
					info.pointer_to_raw_data = s.get_pointer_to_raw_data();
					//Размер файловых данных
					info.size_of_raw_data = s.get_size_of_raw_data();
					//Относительный виртуальный адрес секции
					info.virtual_address = s.get_virtual_address();
					//Виртуальный размер секции
					info.virtual_size = s.get_virtual_size();

					//Копируем имя секции (оно максимально 8 символов)
					memset(info.name, 0, sizeof(info.name));
					memcpy(info.name, s.get_name().c_str(), s.get_name().length());
				}

				//Если секция пустая, переходим к следующей
				if(s.get_raw_data().empty())
					continue;

				//А если не пустая - копируем ее данные в строку
				//с данными всех секций
				raw_section_data += s.get_raw_data();
			}

			//Если все секции оказались пустыми, то паковать нечего!
			if(raw_section_data.empty())
			{
				log_file << "All sections of PE file are empty!" << std::endl;
				LogEdit(L"All sections of PE file are empty!\r\n");
				return -1;
			}

			packed_sections_info += raw_section_data;
		}


		//Новая секция
		section new_section;
		//Имя - .rsrc (пояснение ниже)
		new_section.set_name(".rsrc");
		//Доступна на чтение, запись, исполнение
		new_section.readable(true).writeable(true).executable(true);
		//Ссылка на сырые данные секции
		std::string& out_buf = new_section.get_raw_data();


		//Создаем "умный" указатель
		//и выделяем необходимую для сжатия алгоритму LZO память
		//Умный указатель в случае чего автоматически
		//эту память освободит
		//Мы используем тип lzo_align_t для того, чтобы
		//память была выровняна как надо
		//(из документации к LZO)
		boost::scoped_array<lzo_align_t> work_memory(new lzo_align_t[LZO1Z_999_MEM_COMPRESS]);

		//Длина неупакованных данных
		lzo_uint src_length = packed_sections_info.size();
		//Сохраним ее в нашу структуру информации о файле
		basic_info.size_of_unpacked_data = src_length;

		//Длина упакованных данных
		//(пока нам неизвестна)
		lzo_uint out_length = 0;

		//Необходимый буфер для сжатых данных
		//(длина опять-таки исходя из документации к LZO)
		out_buf.resize(src_length + src_length / 16 + 64 + 3);

		//Производим сжатие данных
		log_file << "Packing data..." << std::endl;
		LogEdit(L"Packing data...\r\n");
		if (LZO_E_OK !=
			lzo1z_999_compress(reinterpret_cast<const unsigned char*>(packed_sections_info.data()),
			src_length,
			reinterpret_cast<unsigned char*>(&out_buf[0]),
			&out_length,
			work_memory.get())
			)
		{
			//Если что-то не так, выйдем
			log_file << "Error compressing data!" << std::endl;
			LogEdit(L"Error compressing data!\r\n");
			return -1;
		}

		log_file << "Packing complete..." << std::endl;
		LogEdit(L"Packing complete..\r\n");
		//Сохраним длину упакованных данных в нашу структуру
		basic_info.size_of_packed_data = out_length;
		//Ресайзим выходной буфер со сжатыми данными по
		//результирующей длине сжатых данных, которая
		//теперь нам известна
		out_buf.resize(out_length);

		//шифрование
		/*		
		if (encrypt){			
			BYTE * out;
			BYTE * in = (BYTE*)out_buf.c_str();
			// key
			unsigned char key[b] = { 110, 36, 2, 15, 3, 17, 24, 23, 18, 45, 1, 21, 122, 16, 3, 12 };			
			int size = crypt(in, out_buf.size() , key, out);
			//PBYTE out2;
			//decrypt(out, size, key, out2); 
			out_buf = std::string();
			out_buf.resize(size);
			for (int i = 0; i < size; i++)
			{
				out_buf[i] = (char)out[i];
			}
			basic_info.size_of_crypted_data = size;			
		}
		*/
		/*
		HANDLE hFile = CreateFile(L"uncrypteddata", GENERIC_WRITE, 0, NULL,
				CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		DWORD nBytes;
		WriteFile(hFile, reinterpret_cast<LPCVOID>(out_buf.c_str()), out_buf.size(), &nBytes, NULL);
			CloseHandle(hFile);*/
	//	int size_packed = out_buf.size();
		basic_info.size_of_crypted_data = basic_info.size_of_packed_data;
		basic_info.iv1 = 0;
		basic_info.iv2 = 0;
		if (encrypt){
			log_file << "Encryption data..." << std::endl;
			LogEdit(L"Encryption data...\r\n");
			//RC5
			if (opt.rc5){
				LogEdit(L"RC5\r\n");
				log_file << "RC5" << std::endl;
				unsigned char key[b] = { 110, 36, 2, 15, 3, 17, 24, 23, 18, 45, 1, 21, 122, 16, 3, 12 };
				srand(time(0));
				unsigned long int iv[2] = { rand(), rand() };
				basic_info.iv1 = iv[0];
				basic_info.iv2 = iv[1];
				basic_info.size_of_crypted_data = ncrypt(out_buf, key, iv);
				basic_info.crypt_mode = 2;
				//	ndecrypt(out_buf, key);
				//	out_buf.resize(size_packed);
			}
			else {
				// XOR
				LogEdit(L"XOR\r\n");
				log_file << "XOR" << std::endl;
				unsigned char key[b] = { 110, 36, 2, 15, 3, 17, 24, 23, 18, 45, 1, 21, 122, 16, 3, 12 };
				nxor_crypt(out_buf, key, b);
				basic_info.crypt_mode = 1;

			}
			log_file << "Success encryption data..." << std::endl;
			LogEdit(L"Success encryption data...\r\n");
		}
		else
		{
			basic_info.crypt_mode = 0;
			log_file << "Encryption data off." << std::endl;
			LogEdit(L"Encryption data off.\r\n");
		}
		if (opt.anti_debug){
			basic_info.anti_debug = 1;
			log_file << "Anti-debug active." << std::endl;
			LogEdit(L"Anti-debug active.\r\n");
		}
		else{
			basic_info.anti_debug = 0;
			log_file << "Anti-debug off." << std::endl;
			LogEdit(L"Anti-debug off.\r\n");
		}
		//Собираем буфер воедино, это и будут
		//финальные данные нашей новой секции
		out_buf =
			//Данные структуры basic_info
			std::string(reinterpret_cast<const char*>(&basic_info), sizeof(basic_info))
			//Выходной буфер
			+ out_buf;

		//Проверим, что файл реально стал меньше
		if(out_buf.size() >= src_length)
		{
			log_file << "File is incompressible!" << std::endl;
			LogEdit(L"File is incompressible!\r\n");
			if (!force_mode)
				return -1;
		}


		//Если файл имеет TLS, получим информацию о нем
		std::auto_ptr<tls_info> tls;
		if(image.has_tls())
		{
			log_file << "Reading TLS..." << std::endl;
			LogEdit(L"Reading TLS...\r\n");
			tls.reset(new tls_info(get_tls_info(image)));
		}


		//Если файл имеет экспорты, получим информацию о них
		//и их список
		exported_functions_list exports;
		export_info exports_info;
		if(image.has_exports())
		{
			log_file << "Reading exports..." << std::endl;
			LogEdit(L"Reading exports...\r\n");
			exports = get_exported_functions(image, exports_info);
		}


		//Если файл имеет Image Load Config, получим информацию о ней
		std::auto_ptr<image_config_info> load_config;
		if(image.has_config() && rebuild_load_config)
		{
			log_file << "Reading Image Load Config..." << std::endl;
			LogEdit(L"Reading Image Load Config...\r\n");
			try
			{
				load_config.reset(new image_config_info(get_image_config(image)));
			}
			catch(const pe_exception& e)
			{
				image.remove_directory(IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG);
				log_file << "Error reading load config directory: " << e.what() << std::endl;
				LogEdit(L"Reading Image Load Config...\r\n");
			}
		}
		else
		{
			image.remove_directory(IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG);
		}

		{
			//Сначала получим ссылку на самю первую
			//существующую секцию PE-файла
			const section& first_section = image.get_image_sections().front();
			//Установим виртуальный адрес для добавляемой секции (читай ниже)
			new_section.set_virtual_address(first_section.get_virtual_address());

			//Теперь получим ссылку на самю последнюю
			//существующую секцию PE-файла
			const section& last_section = image.get_image_sections().back();
			//Посчитаем общий размер виртуальных данных
			DWORD total_virtual_size = 
				//Виртуальный адрес последней секции
				last_section.get_virtual_address()
				//Выровненный виртуальный размер последней секции
				+ pe_utils::align_up(last_section.get_virtual_size(), image.get_section_alignment())
				//Минус виртуальный размер первой секции
				- first_section.get_virtual_address();

			//Новая пустая корневая директория ресурсов
			resource_directory new_root_dir;

			if(image.has_resources() && repack_resources)
			{
				log_file << "Repacking resources..." << std::endl;
				LogEdit(L"Repacking resources...\r\n");
				//Получим ресурсы исходного файла (корневую директорию)
				resource_directory root_dir = get_resources(image);
				//Оборачиваем оригинальную и новую директорию ресурсов
				//во вспомогательные классы
				pe_resource_viewer res(root_dir);
				pe_resource_manager new_res(new_root_dir);

				try
				{
					//Перечислим все именованные группы иконок
					//и группы иконок, имеющие ID
					pe_resource_viewer::resource_id_list icon_id_list(res.list_resource_ids(pe_resource_viewer::resource_icon_group));
					pe_resource_viewer::resource_name_list icon_name_list(res.list_resource_names(pe_resource_viewer::resource_icon_group));
					//Сначала всегда располагаются именованные ресурсы, поэтому проверим, есть ли они
					if(!icon_name_list.empty())
					{
						//Получим самую первую иконку для самого первого языка (по индексу 0)
						//Если надо было бы перечислить языки для заданной иконки, можно было вызвать list_resource_languages
						//Если надо было бы получить иконку для конкретного языка, можно было вызвать get_icon_by_name (перегрузка с указанием языка)
						//Добавим группу иконок в новую директорию ресурсов
						resource_cursor_icon_writer(new_res).add_icon(
							resource_cursor_icon_reader(res).get_icon_by_name(icon_name_list[0]),
							icon_name_list[0],
							res.list_resource_languages(pe_resource_viewer::resource_icon_group, icon_name_list[0]).at(0));
					}
					else if(!icon_id_list.empty()) //Если нет именованных групп иконок, но есть группы с ID
					{
						//Получим самую первую иконку для самого первого языка (по индексу 0)
						//Если надо было бы перечислить языки для заданной иконки, можно было вызвать list_resource_languages
						//Если надо было бы получить иконку для конкретного языка, можно было вызвать get_icon_by_id_lang
						//Добавим группу иконок в новую директорию ресурсов
						resource_cursor_icon_writer(new_res).add_icon(
							resource_cursor_icon_reader(res).get_icon_by_id(icon_id_list[0]),
							icon_id_list[0],
							res.list_resource_languages(pe_resource_viewer::resource_icon_group, icon_id_list[0]).at(0));
					}
				}
				catch(const pe_exception&)
				{
					//Если какая-то ошибка с ресурсами, например, иконок нет,
					//то ничего не делаем
				}

				try
				{
					//Получим список манифестов, имеющих ID
					pe_resource_viewer::resource_id_list manifest_id_list(res.list_resource_ids(pe_resource_viewer::resource_manifest));
					if(!manifest_id_list.empty()) //Если манифест есть
					{
						//Получим самый первый манифест для самого первого языка (по индексу 0)
						//Добавим манифест в новую директорию ресурсов
						new_res.add_resource(
							res.get_resource_data_by_id(pe_resource_viewer::resource_manifest, manifest_id_list[0]).get_data(),
							pe_resource_viewer::resource_manifest,
							manifest_id_list[0],
							res.list_resource_languages(pe_resource_viewer::resource_manifest, manifest_id_list[0]).at(0)
							);
					}
				}
				catch(const pe_exception&)
				{
					//Если какая-то ошибка с ресурсами,
					//то ничего не делаем
				}

				try
				{
					//Получим список структур информаций о версии, имеющих ID
					pe_resource_viewer::resource_id_list version_info_id_list(res.list_resource_ids(pe_resource_viewer::resource_version));
					if(!version_info_id_list.empty()) //Если информация о версии есть
					{
						//Получим самую первую структуру информации о версии для самого первого языка (по индексу 0)
						//Добавим информацию о версии в новую директорию ресурсов
						new_res.add_resource(
							res.get_resource_data_by_id(pe_resource_viewer::resource_version, version_info_id_list[0]).get_data(),
							pe_resource_viewer::resource_version,
							version_info_id_list[0],
							res.list_resource_languages(pe_resource_viewer::resource_version, version_info_id_list[0]).at(0)
							);
					}
				}
				catch(const pe_exception&)
				{
					//Если какая-то ошибка с ресурсами,
					//то ничего не делаем
				}
			}


			//Удаляем все секции PE-файла
			image.get_image_sections().clear();

			//Изменяем файловое выравнивание
			image.realign_file(file_alignment);

			//Добавляем нашу секцию и получаем ссылку на
			//уже добавленную секцию с пересчитанными адресами и размерами
			section& added_section = image.add_section(new_section);
			//Устанавливаем для нее необходимый виртуальный размер
			image.set_section_virtual_size(added_section, total_virtual_size);


			log_file << "Creating imports..." << std::endl;
			LogEdit(L"Creating imports...\r\n");
			//Создаем импорты из библиотеки kernel32.dll
			import_library kernel32;
			kernel32.set_name("kernel32.dll"); //Выставили имя библиотеки

			//Создаем импортируемую функцию
			imported_function func;
			func.set_name("LoadLibraryA"); //Ее имя
			kernel32.add_import(func); //Добавляем ее к библиотеке

			//И вторую функцию
			func.set_name("GetProcAddress");
			kernel32.add_import(func); //Тоже добавляем

			//Получаем относительный адрес (RVA) поля load_library_a
			//нашей структуры packed_file_info, которую мы расположили в самом
			//начале добавленной секции, помните?
			DWORD load_library_address_rva = pe_base::rva_from_section_offset(added_section,
				offsetof(packed_file_info, load_library_a));

			//Устанавливаем этот адрес как адрес
			//таблицы адресов импорта (import address table)
			kernel32.set_rva_to_iat(load_library_address_rva);

			//Создаем список импортируемых библиотек
			imported_functions_list imports;
			//Добавляем к списку нашу библиотеку
			imports.push_back(kernel32);

			//Настроим пересборщик импортов
			import_rebuilder_settings settings;
			//Original import address table нам не нужна (пояснения ниже)
			settings.build_original_iat(false);
			//Будем переписывать IAT именно по тому адресу,
			//которому указали (load_library_address_rva)
			settings.save_iat_and_original_iat_rvas(true, true);
			//Расположим импорты прямо за концом упакованных данных
			settings.set_offset_from_section_start(added_section.get_raw_data().size());

			//Если у нас есть ресурсы для сборки,
			//отключим автоматическое урезание секции после
			//добавления в нее импортов
			if(!new_root_dir.get_entry_list().empty())
				settings.enable_auto_strip_last_section(false);

			//Пересоберем импорты
			rebuild_imports(image, imports, added_section, settings);

			//Пересоберем ресурсы, если есть, что пересобирать
			if(!new_root_dir.get_entry_list().empty())
				rebuild_resources(image, new_root_dir, added_section, added_section.get_raw_data().size());


			//Если у файла был TLS
			if(tls.get())
			{
				//Указатель на нашу структуру с информацией
				//для распаковщика
				//Эта структура в самом начале свежедобавленной секции,
				//мы ее туда добавили чуть раньше
				packed_file_info* info = reinterpret_cast<packed_file_info*>(&added_section.get_raw_data()[0]);

				//Запишем относительный виртуальный адрес
				//оригинального TLS
				info->original_tls_index_rva = tls->get_index_rva();

				//Если у нас были TLS-коллбэки, запишем в структуру
				//относительный виртуальный адрес их массива в оригинальном файле
				if(!tls->get_tls_callbacks().empty())
					info->original_rva_of_tls_callbacks = tls->get_callbacks_rva();

				//Теперь относительный виртуальный адрес индекса TLS
				//будет другим - мы заставим загрузчик записать его в поле tls_index
				//структуры packed_file_info
				tls->set_index_rva(pe_base::rva_from_section_offset(added_section, offsetof(packed_file_info, tls_index)));
			}
		}


		//Смещение относительно начала второй секции
		//к абсолютному адресу TLS-коллбэка
		DWORD first_callback_offset = 0;

		{
			//Новая секция
			section unpacker_section;
			//Имя 
			unpacker_section.set_name("section");
			//Доступна на запись, чтение и исполнение
			unpacker_section.readable(true).executable(true).writeable(true);

			{
				log_file << "Writing unpacker stub, size = " << sizeof(unpacker_data) << " bytes" << std::endl;
		    	LogEdit(L"Writing unpacker stub, size = " + std::to_wstring(sizeof(unpacker_data)) +L" bytes\r\n" );
				
				//Получаем ссылку на данные секции распаковщика
				std::string& unpacker_section_data = unpacker_section.get_raw_data();
				//Записываем туда код распаковщика
				//Этот код хранится в автогенеренном файле
				//unpacker.h, который мы подключили в main.cpp
				unpacker_section_data = std::string(reinterpret_cast<const char*>(unpacker_data), sizeof(unpacker_data));

				//Записываем по нужным смещениям адрес
				//загрузки образа
				*reinterpret_cast<DWORD*>(&unpacker_section_data[original_image_base_offset]) = image.get_image_base_32();
				*reinterpret_cast<DWORD*>(&unpacker_section_data[original_image_base_no_fixup_offset]) = image.get_image_base_32();

				//и виртуальный адрес самой первой секции упакованного файла,
				//в которой лежат данные для распаковки и информация о них
				//В самом начале это секции, как вы помните, лежит
				//структура packed_file_info
				*reinterpret_cast<DWORD*>(&unpacker_section_data[rva_of_first_section_offset]) = image.get_image_sections().at(0).get_virtual_address();
			}

			//Добавляем и эту секцию
			section& unpacker_added_section = image.add_section(unpacker_section);

			if(tls.get() || image.has_exports() || image.has_reloc() || load_config.get())
			{
				//Изменим размер данных секции распаковщика ровно
				//по количеству байтов в теле распаковщика
				//(на случай, если нулевые байты с конца были обрезаны
				//библиотекой для работы с PE)
				unpacker_added_section.get_raw_data().resize(sizeof(unpacker_data));
			}


			//Если у файла есть TLS
			if(tls.get())
			{
				log_file << "Rebuilding TLS..." << std::endl;
				LogEdit(L"Rebuilding TLS...\r\n");
				//Ссылка на сырые данные секции распаковщика
				//Сейчас там есть только тело распаковщика
				std::string& data = unpacker_added_section.get_raw_data();

				//Вычислим позицию, в которую запишем структуру IMAGE_TLS_DIRECTORY32
				DWORD directory_pos = data.size();
				//Выделим место под эту структуру
				//запас sizeof(DWORD) нужен для выравнивания, так как
				//IMAGE_TLS_DIRECTORY32 должна быть выровнена 4-байтовую на границу
				data.resize(data.size() + sizeof(IMAGE_TLS_DIRECTORY32) + sizeof(DWORD));

				//Если у TLS есть коллбэки...
				if(!tls->get_tls_callbacks().empty())
				{
					//Необходимо зарезервировать место
					//под оригинальные TLS-коллбэки
					//Плюс 1 ячейка под нулевой DWORD
					first_callback_offset = data.size();
					data.resize(data.size() + sizeof(DWORD) * (tls->get_tls_callbacks().size() + 1));

					//Первый коллбэк будет нашим пустым (ret 0xC),
					//запишем его адрес
					*reinterpret_cast<DWORD*>(&data[first_callback_offset]) =
						image.rva_to_va_32(pe_base::rva_from_section_offset(unpacker_added_section, empty_tls_callback_offset));

					//Запишем относительный виртуальный адрес
					//новой таблицы TLS-коллбэков
					tls->set_callbacks_rva(pe_base::rva_from_section_offset(unpacker_added_section, first_callback_offset));

					//Теперь запишем в структуру packed_file_info, которую мы
					//записали в самое начало первой секции,
					//относительный адрес новой таблицы коллбэков
					reinterpret_cast<packed_file_info*>(&image.get_image_sections().at(0).get_raw_data()[0])->new_rva_of_tls_callbacks = tls->get_callbacks_rva();
				}
				else
				{
					//Если нет коллбэков, на всякий случай обнулим адрес
					tls->set_callbacks_rva(0);
				}

				//Очистим массив коллбэков, они нам больше не нужны
				//Мы их сделали вручную
				tls->clear_tls_callbacks();

				//Установим новый относительный адрес
				//данных для инициализации локальной памяти потока
				tls->set_raw_data_start_rva(pe_base::rva_from_section_offset(unpacker_added_section, data.size()));
				//Пересчитываем адрес конца этих данных
				tls->recalc_raw_data_end_rva();

				//Пересобираем TLS
				//Указываем пересборщику, что не нужно писать данные и коллбэки
				//Мы сделаем это вручную (коллбэки уже записали, куда надо)
				//Также указываем, что не нужно обрезать нулевые байты в конце секции
				rebuild_tls(image, *tls, unpacker_added_section, directory_pos, false, false, tls_data_expand_raw, true, false);

				//Дополняем секцию данными для инициализации
				//локальной памяти потока
				unpacker_added_section.get_raw_data() += tls->get_raw_data();
				//Теперь установим виртуальный размер добавленной секции 
				//с учетом SizeOfZeroFill поля TLS
				image.set_section_virtual_size(unpacker_added_section, data.size() + tls->get_size_of_zero_fill());

				//Наконец, обрежем уже ненужные нулевые байты с конца секции
				if(!image.has_reloc() && !image.has_exports() && !load_config.get())
					pe_utils::strip_nullbytes(unpacker_added_section.get_raw_data());

				//и пересчитаем ее размеры (физический и виртуальный)
				image.prepare_section(unpacker_added_section);
			}


			//Выставляем новую точку входа - теперь она указывает
			//на распаковщик, на самое его начало
			image.set_ep(image.rva_from_section_offset(unpacker_added_section, 0) + 0x5C); //0x5c Смещение от начала
		}

		if(load_config.get())
		{
			log_file << "Repacking load configuration..." << std::endl;
			LogEdit(L"Repacking load configuration...\r\n");
			section& unpacker_section = image.get_image_sections().at(1);

			//Обнулим таблицу адресов Lock-префиксов
			load_config->clear_lock_prefix_list();
			load_config->add_lock_prefix_rva(pe_base::rva_from_section_offset(image.get_image_sections().at(0), offsetof(packed_file_info, lock_opcode)));

			//Пересобираем директорию конфигурации загрузки и располагаем ее в секции
			//Пересобираем автоматически таблицу SE Handler'ов, а вот таблицу Lock-префиксов не создаем
			rebuild_image_config(image, *load_config, unpacker_section, unpacker_section.get_raw_data().size(), true, true, true, !image.has_reloc() && !image.has_exports());
		}

		//Если у файла есть релокации
		if(image.has_reloc())
		{
			log_file << "Creating relocations..." << std::endl;
			LogEdit(L"Creating relocations...\r\n");
			//Создаем список таблиц релокаций и единственную таблицу
			relocation_table_list reloc_tables;

			section& unpacker_section = image.get_image_sections().at(1);

			{
				relocation_table table;
				//Устанавливаем виртуальный адрес таблицы релокаций
				//Он будет равен относительному виртуальному адресу второй добавленной
				//секции, так как именно в ней находится код распаковщика
				//с переменной, которую мы будем фиксить
				table.set_rva(unpacker_section.get_virtual_address());

				//Добавляем релокацию по смещению original_image_base_offset из
				//файла parameters.h распаковщика
				table.add_relocation(relocation_entry(original_image_base_offset, IMAGE_REL_BASED_HIGHLOW));

				//Добавляем таблицу в список таблиц
				reloc_tables.push_back(table);
			}

			//Если у файла был TLS
			if(tls.get())
			{
				//Просчитаем смещение к структуре TLS
				//относительно начала второй секции
				DWORD tls_directory_offset = image.get_directory_rva(IMAGE_DIRECTORY_ENTRY_TLS)
					- image.section_from_directory(IMAGE_DIRECTORY_ENTRY_TLS).get_virtual_address();
				
				//Создаем новую таблицу релокации, так как область с таблицей TLS может быть сильно удалена
				//от original_image_base_offset
				relocation_table table;
				table.set_rva(image.get_directory_rva(IMAGE_DIRECTORY_ENTRY_TLS));
				//Добавим релокации для полей StartAddressOfRawData,
				//EndAddressOfRawData и AddressOfIndex
				//Эти поля у нас всегда ненулевые
				table.add_relocation(relocation_entry(static_cast<WORD>(offsetof(IMAGE_TLS_DIRECTORY32, StartAddressOfRawData)), IMAGE_REL_BASED_HIGHLOW));
				table.add_relocation(relocation_entry(static_cast<WORD>(offsetof(IMAGE_TLS_DIRECTORY32, EndAddressOfRawData)), IMAGE_REL_BASED_HIGHLOW));
				table.add_relocation(relocation_entry(static_cast<WORD>(offsetof(IMAGE_TLS_DIRECTORY32, AddressOfIndex)), IMAGE_REL_BASED_HIGHLOW));

				//Если имеются TLS-коллбэки
				if(first_callback_offset)
				{
					//То добавим еще релокации для поля AddressOfCallBacks
					//и для адреса нашего пустого коллбэка
					table.add_relocation(relocation_entry(static_cast<WORD>(offsetof(IMAGE_TLS_DIRECTORY32, AddressOfCallBacks)), IMAGE_REL_BASED_HIGHLOW));
					table.add_relocation(relocation_entry(static_cast<WORD>(tls->get_callbacks_rva() - table.get_rva()), IMAGE_REL_BASED_HIGHLOW));
				}

				reloc_tables.push_back(table);
			}

			if(load_config.get())
			{
				//Если файл имеет IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG, то следует добавить необходимые релокации для нее,
				//потому что она используется загрузчиком на этапе загрузки PE-файла
				DWORD config_directory_offset = image.get_directory_rva(IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG)
					- image.section_from_directory(IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG).get_virtual_address();

				//Создаем новую таблицу релокации, так как область с таблицей TLS может быть сильно удалена
				//от original_image_base_offset или TLS
				relocation_table table;
				table.set_rva(image.get_directory_rva(IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG));

				if(load_config->get_security_cookie_va())
					table.add_relocation(relocation_entry(static_cast<WORD>(offsetof(IMAGE_LOAD_CONFIG_DIRECTORY32, SecurityCookie)), IMAGE_REL_BASED_HIGHLOW));

				if(load_config->get_se_handler_table_va())
					table.add_relocation(relocation_entry(static_cast<WORD>(offsetof(IMAGE_LOAD_CONFIG_DIRECTORY32, SEHandlerTable)), IMAGE_REL_BASED_HIGHLOW));

				table.add_relocation(relocation_entry(static_cast<WORD>(offsetof(IMAGE_LOAD_CONFIG_DIRECTORY32, LockPrefixTable)), IMAGE_REL_BASED_HIGHLOW));
				reloc_tables.push_back(table);
			}

			//Пересобираем релокации, располагая их в конце
			//секции с кодом распаковщика
			rebuild_relocations(image, reloc_tables, unpacker_section, unpacker_section.get_raw_data().size(), true, !image.has_exports());
		}

		if(image.has_exports())
		{
			log_file << "Repacking exports..." << std::endl;
			LogEdit(L"Repacking exports...\r\n");
			section& unpacker_section = image.get_image_sections().at(1);

			//Пересобираем экспорты и располагаем их в секции 
			rebuild_exports(image, exports_info, exports, unpacker_section, unpacker_section.get_raw_data().size(), true);
		}

		//Удалим все часто используемые директории
		//В дальнейшем мы будем их возвращать обратно
		//и корректно обрабатывать, но пока так
		image.remove_directory(IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT);
		image.remove_directory(IMAGE_DIRECTORY_ENTRY_IAT);
		image.remove_directory(IMAGE_DIRECTORY_ENTRY_SECURITY);
		image.remove_directory(IMAGE_DIRECTORY_ENTRY_DEBUG);

		//Урезаем таблицу директорий, удаляя все нулевые
		//Урезаем не полностью, а минимум до 12 элементов, так как в оригинальном
		//файле могут присутствовать первые 12 и использоваться
		//image.strip_data_directories(16 - 4); //Закомментировали из-за непереносимости в WinXP
		//Удаляем стаб из заголовка, если какой-то был
		image.strip_stub_overlay();


		std::wstring base_file_name;

		if (output_file_name.empty())
		{
			//Создаем новый PE-файл
			//Вычислим имя переданного нам файла без директории
			base_file_name = input_file_name;
			std::wstring dir_name;
			std::wstring::size_type slash_pos;
			if ((slash_pos = base_file_name.find_last_of(L"/\\")) != std::string::npos)
			{
				dir_name = base_file_name.substr(0, slash_pos + 1); //Директория исходного файла
				base_file_name = base_file_name.substr(slash_pos + 1); //Имя исходного файла
			}

			//Дадим новому файлу имя packed_ + имя_оригинального_файла
			//Вернем к нему исходную директорию, чтобы сохранить
			//файл туда, где лежит оригинал
			base_file_name = dir_name + L"packed_" + base_file_name;
		}
		else
		{
			base_file_name = output_file_name;
		}


		//Создадим файл
		std::ofstream new_pe_file(base_file_name.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
		if (!new_pe_file)
		{
			//Если не удалось создать файл - выведем ошибку
			log_file << "Cannot create " << to_utf8(base_file_name) << std::endl;
			LogEdit(L"Cannot create " + base_file_name + L"\r\n");
			return -1;
		}

		//Пересобираем PE-образ
		//Урезаем DOS-заголовок, накладывая на него NT-заголовки
		//(за это отвечает второй параметр true)
		//Не пересчитываем SizeOfHeaders - за это отвечает третий параметр
		rebuild_pe(image, new_pe_file, strip_dos_headers, false);

		//Оповестим пользователя, что файл упакован успешно
		log_file << "Packed image was saved to " << to_utf8(base_file_name) << std::endl;
		LogEdit(L"Packed image was saved to " + base_file_name + L"\r\n");
		log_file << "Resulting sections entropy: " << entropy_calculator::calculate_entropy(image) << std::endl;	
		LogEdit(L"Packed image was saved to " + std::to_wstring(entropy_calculator::calculate_entropy(image)) + L"\r\n");
		log_file << "Finished in " << pack_timer.elapsed() << " sec" << std::endl;
		LogEdit(L"Finished in " + std::to_wstring(pack_timer.elapsed()) + L" sec\r\n");
	}
	catch (const pe_exception& e)
	{
		//Если по какой-то причине открыть его не удалось
		//Выведем текст ошибки и выйдем
		log_file << "Error: " << e.what() << std::endl;
		LogEdit(L"Error\r\n");
		return -1;
	}
	log_file.close();
	return 0;
}
