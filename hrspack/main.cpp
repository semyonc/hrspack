//
//  hrspack is an experimental lossless codec and archiver 
//
//  Copyright (c) 2014-2016 Semyon A. Chertkov. All rights reserved.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
// http://www.gnu.org/copyleft/gpl.html



#include "stdafx.h"
#include "hrspack.h"

// Null buffer for no output
class NullBuffer : public std::streambuf
{
public:
	int overflow(int c) { return c; }
};

std::ostream *verbose_output;
int level = DEFAULT_OPTION;  // Compression level 0 to 9
bool calculateCRC = true;

int main(int argc, const char * argv[]) {
	if (argc == 1) {
		showUsage();
		return RETURN_CODE_OK;
	}
	
	if (CheckOption(argc, argv, "-h")) {
		showHelp();
		return RETURN_CODE_OK;
	}

	NullBuffer null_buffer;
	std::ostream null_stream(&null_buffer);;
	if (CheckOption(argc, argv, "-v")) {
		verbose_output = &std::cout;
	}
	else {
		verbose_output = &null_stream;
	}

	if (CheckOption(argc, argv, "-x")) { // decompress

		std::string inputFileName;
		std::string outputFileName;
		if (*argv[argc - 1] != '-') {
			if (argc > 2 && *argv[argc - 2] != '-') {
				inputFileName = argv[argc - 2];
				outputFileName = argv[argc - 1];
			}
			else {
				inputFileName = argv[argc - 1];
				outputFileName = "";  // default ext from file type
			}
		}
		else {
			showUsage();
			return RETURN_CODE_OK;
		}

		int ret = decompress(inputFileName, outputFileName);
		if (ret == RETURN_CODE_CRC_MISMATCH) {
			remove(outputFileName.c_str());
		}

		return ret;
	}
	else { // compress
		level = GetOptionValue(argc, argv, "-m", DEFAULT_OPTION);
		if (level < 1 || level > 9) {
			std::cerr << "Incorrect memory option value. Value should be set from -m1 to -m9";
			return RETURN_CODE_BAD_ARGUMENT;
		}

		if (CheckOption(argc, argv, "-e")) {
			calculateCRC = false;
		}

		std::string inputFileName;
		std::string outputFileName;
		if (*argv[argc - 1] != '-') {
			if (argc > 2 && *argv[argc - 2] != '-') {
				inputFileName = argv[argc - 2];
				outputFileName = argv[argc - 1];
			}
			else {
				inputFileName = argv[argc - 1];
				size_t lastindex = inputFileName.find_last_of(".");
				std::string rawname = inputFileName.substr(0, lastindex);
				outputFileName = rawname.append(FILE_EXT);
			}
		}
		else {
			showUsage();
			return RETURN_CODE_OK;
		}

		size_t lastindex = inputFileName.find_last_of(".");
		std::string ext = inputFileName.substr(lastindex);
		std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

		int ret;
		if (ext == ".aif")
			ret = compressAIFF(inputFileName, outputFileName);
		else {
			std::cerr << "Unknown input file extension " << ext << std::endl;
			return RETURN_CODE_BAD_ARGUMENT;
		}

		if (ret == RETURN_CODE_OK) {
			if (CheckOption(argc, argv, "-c")) { // check accuracy
				COUT << "+----------------+" << std::endl;
				COUT << ": Accuracy check :" << std::endl;
				COUT << "+----------------+" << std::endl;
				std::string testFileName = std::tmpnam(nullptr);
				ret = decompress(outputFileName, testFileName);
				if (ret == RETURN_CODE_OK || ret == RETURN_CODE_CRC_MISMATCH) {
					if (!compareFile(inputFileName, testFileName))
						ret = RETURN_CODE_VALIDATION;
				}
				remove(testFileName.c_str());
				if (ret != RETURN_CODE_OK) {
					remove(outputFileName.c_str());
				}
			}
			if (ret == 0 && CheckOption(argc, argv, "-d")) { // delete source file
				if (remove(inputFileName.c_str()) != 0) {
					ret = RETURN_CODE_IO_ERROR;
				}
				COUT << "Delete file " << getfilename(inputFileName) << std::endl;
			}
		}
		else { // delete resulting file
			remove(outputFileName.c_str());
		}

		return ret;
	}
}

// Binary files comparing
bool compareFile(const std::string &file1, const std::string &file2)
{
	std::ifstream ifs1(file1, std::ifstream::binary);
	if (!ifs1.good()) {
		std::cerr << "Can't open for comparing file1 " << file1 << std::endl;
		return false;
	}
	std::ifstream ifs2(file2, std::ifstream::binary);
	if (!ifs2.good()) {
		std::cerr << "Can't open for comparing file2 " << file2 << std::endl;
		ifs1.close();
		return false;
	}
	ifs1.seekg(0, ifs1.end);
	ifs2.seekg(0, ifs2.end);
	LONG length = ifs1.tellg();
	if (ifs1.tellg() != ifs2.tellg()) {
		COUT << "Compared files have different length (" <<
			ifs1.tellg() << ", " << ifs2.tellg() << ")" << std::endl;
		ifs1.close();
		ifs2.close();
		return false;
	}
	ifs1.seekg(0);
	ifs2.seekg(0);
	LONG offset = 0;
	char b1[CHAR_BUFFER_SIZE];
	char b2[CHAR_BUFFER_SIZE];
	while (!ifs1.eof() && !ifs2.eof()) {
		ifs1.read(&b1[0], sizeof(b1));
		ifs2.read(&b2[0], sizeof(b2));
		for (int i = 0; i < ifs1.gcount(); i++) {
			if (b1[i] != b2[i]) {
				COUT << "Compare... FAILED !" << std::endl;
				std::cerr << "Compared files are different at offset = " << offset << std::endl;
				ifs1.close();
				ifs2.close();
				return false;
			}
			offset++;
		}
	}
	COUT << "Compare... OK" << std::endl;
	ifs1.close();
	ifs2.close();
	return true;
}

// Search for arbitrary option
// Returns true if option is set, otherwise false
bool CheckOption(int argc, const char **argv, const char *opt)
{
	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], opt))
			return true;		
	}
	return false;
}

// Get value of an arbitrary option
// Returns value, e.g. 3 if option is "-a3"
// Returns 0 if option is set but has no value (e.g. "-a")
int GetOptionValue(int argc, const char **argv, const char *opt, int default_value)
{
	int i;
	size_t	optLen = strlen(opt);

	for (i = 1; i<argc; ++i) {
		if (!strncmp(argv[i], opt, optLen)) {
			int val;
			if (sscanf(argv[i] + optLen, "%d", &val) == 1) {
				// Return the value
				return val;
			}
			// Ignore if not in "-a#" format
		}
	}
	return default_value;
}

// Get multiple values of an arbitrary option, e.g. -m1,4,3,2,5
// Returns number of values read (exception: N < 1 returns -1 if option is set)
// The values have to be separated by commas, they are returned in val[0..N-1]
// (make sure that memory for val has been allocated)
int GetOptionValues(int argc, const char **argv, const char *opt, int N, int *val)
{
	int i, n;
	const char *str, *pos;
	size_t	OptLen = strlen(opt);

	i = 1;
	while (i < argc)
	{
		if (!strncmp(argv[i], opt, OptLen))
		{
			if (N < 1)		// Check if option is set
				return -1;	// Option is set, values are ignored

			str = argv[i] + OptLen;

			val[0] = atoi(str);		// First value
			n = 1;

			// Further values are separated by commas
			while ((n < N) && ((pos = strchr(str, ',')) != NULL))
			{
				str = pos + 1;			// Position after comma
				val[n] = atoi(str);		// Value
				n++;
			}

			return n;	// Number of values read (max. N)
		}
		else
			i++;
	}
	return 0;			// Option not set
}

void showUsage()
{
	std::cerr << "Usage: " << APP_NAME << " [options] infile [outfile]" << std::endl;
	std::cerr << "For help, type: " << APP_NAME << " -h" << std::endl;
}

void showHelp()
{
	std::cout << APP_NAME << " is an experimental lossless codec and archiver" << std::endl;
	std::cout << "  Version 1.0 for " << SYSTEM_STR << std::endl;
	std::cout << "  (c) 2014-2016 Semyon A. Chertkov" << std::endl;
	std::cout << "  E-mail: semyonc@gmail.com" << std::endl;
	std::cout << "  Uses portion of code paq8f" << std::endl;
	std::cout << "     (c) 2007, Matt Mahoney" << std::endl;
	std::cout << "  Uses portion of code Monkey's Audio SDK" << std::endl;
	std::cout << "     (c) 2000-2013, Matthew T. Ashland." << std::endl;
	std::cout << "  Uses portion of code MPEG - 4 Audio Lossless Coding(ALS), Reference Model Codec" << std::endl;
	std::cout << "     (c) 2003-2008 Tilman Liebchen, TU Berlin / LG Electronics." << std::endl;
	std::cout << std::endl;
	std::cout << "Usage: " << APP_NAME << " [options] infile [outfile]" << std::endl;
	std::cout << "  In compression mode infile must be an AIFF file" << std::endl;
	std::cout << "  Mono, stereo, and multichannel files and up to" << std::endl;
	std::cout << "  32-bit resolution are supported at any sampling frequency." << std::endl;
	std::cout << "  In decompression mode (-x), infile is the compressed file (" << FILE_EXT << ")." << std::endl;
	std::cout << "  If outfile is not specified, the name of the output file will be generated" << std::endl;
	std::cout << "  by replacing the extension of the input file." << std::endl;
	std::cout << "General Options:" << std::endl;
	std::cout << "  -c  : Check accuracy by decoding the whole file after encoding" << std::endl;
	std::cout << "  -d  : Delete input file after completion" << std::endl;
	std::cout << "  -h  : Help (this message)" << std::endl;
	std::cout << "  -v  : Verbose mode (file info, processing time)" << std::endl;
	std::cout << "  -x  : Extract (all options except -v are ignored)" << std::endl;
	std::cout << "Encoding Options:" << std::endl;
	std::cout << "  -m# : Set memory size for prediction (Minimum: 1, Maximum 9 (extra large), 3 is a default)" << std::endl;
	std::cout << "  -e  : Exclude CRC calculation" << std::endl;
	std::cout << std::endl;
	std::cout << "Examples:" << std::endl;
	std::cout << "  " << APP_NAME << " -v demo.aif" << std::endl;
	std::cout << "  " << APP_NAME << " -x demo" << FILE_EXT << std::endl;
	std::cout << std::endl;
}

