#include <Poco/SharedPtr.h>

#include <DB/Columns/ColumnArray.h>
#include <DB/Columns/ColumnString.h>
#include <DB/Columns/ColumnsNumber.h>
#include <DB/Columns/ColumnConst.h>

#include <DB/DataTypes/DataTypeString.h>

#include <DB/IO/ReadHelpers.h>
#include <DB/IO/WriteHelpers.h>
#include <DB/IO/VarInt.h>

#define DBMS_APPROX_STRING_SIZE 64


namespace DB
{

using Poco::SharedPtr;


void DataTypeString::serializeBinary(const Field & field, WriteBuffer & ostr) const
{
	const String & s = get<const String &>(field);
	writeVarUInt(s.size(), ostr);
	writeString(s, ostr);
}


void DataTypeString::deserializeBinary(Field & field, ReadBuffer & istr) const
{
	UInt64 size;
	readVarUInt(size, istr);
	field = String();
	String & s = get<String &>(field);
	s.resize(size);
	/// непереносимо, но (действительно) быстрее
	istr.readStrict(const_cast<char*>(s.data()), size);
}


void DataTypeString::serializeBinary(const IColumn & column, WriteBuffer & ostr, size_t offset, size_t limit) const
{
	const ColumnArray & column_array = dynamic_cast<const ColumnArray &>(column);
	const ColumnUInt8::Container_t & data = dynamic_cast<const ColumnUInt8 &>(column_array.getData()).getData();
	const ColumnArray::Offsets_t & offsets = column_array.getOffsets();

	size_t array_size = column_array.size();
	size_t size = column.size();
	if (!array_size)
		return;

	size_t end = limit && offset + limit < size
		? offset + limit
		: size;

	if (offset == 0)
	{
		UInt64 str_size = offsets[0] - 1;
		writeVarUInt(str_size, ostr);
		ostr.write(reinterpret_cast<const char *>(&data[0]), str_size);
		
		++offset;
	}
	
	for (size_t i = offset; i < end; ++i)
	{
		UInt64 str_size = offsets[i] - offsets[i - 1] - 1;
		writeVarUInt(str_size, ostr);
		ostr.write(reinterpret_cast<const char *>(&data[offsets[i - 1]]), str_size);
	}
}


void DataTypeString::deserializeBinary(IColumn & column, ReadBuffer & istr, size_t limit) const
{
	ColumnArray & column_array = dynamic_cast<ColumnArray &>(column);
	ColumnUInt8::Container_t & data = dynamic_cast<ColumnUInt8 &>(column_array.getData()).getData();
	ColumnArray::Offsets_t & offsets = column_array.getOffsets();

	data.reserve(limit * DBMS_APPROX_STRING_SIZE);
	offsets.reserve(limit);

	size_t offset = 0;
	for (size_t i = 0; i < limit; ++i)
	{
		if (istr.eof())
			break;

		UInt64 size;
		readVarUInt(size, istr);

		offset += size + 1;
		offsets.push_back(offset);

		if (data.size() < offset)
			data.resize(offset);
		
		istr.readStrict(reinterpret_cast<char*>(&data[offset - size - 1]), sizeof(ColumnUInt8::value_type) * size);
		data[offset - 1] = 0;
	}
}


void DataTypeString::serializeText(const Field & field, WriteBuffer & ostr) const
{
	writeString(get<const String &>(field), ostr);
}


void DataTypeString::deserializeText(Field & field, ReadBuffer & istr) const
{
	field.assignString("", 0);
	readString(get<String &>(field), istr);
}


void DataTypeString::serializeTextEscaped(const Field & field, WriteBuffer & ostr) const
{
	writeEscapedString(get<const String &>(field), ostr);
}


void DataTypeString::deserializeTextEscaped(Field & field, ReadBuffer & istr) const
{
	field.assignString("", 0);
	readEscapedString(get<String &>(field), istr);
}


void DataTypeString::serializeTextQuoted(const Field & field, WriteBuffer & ostr) const
{
	writeQuotedString(get<const String &>(field), ostr);
}


void DataTypeString::deserializeTextQuoted(Field & field, ReadBuffer & istr) const
{
	field.assignString("", 0);
	readQuotedString(get<String &>(field), istr);
}


ColumnPtr DataTypeString::createColumn() const
{
	return new ColumnString;
}


ColumnPtr DataTypeString::createConstColumn(size_t size, const Field & field) const
{
	return new ColumnConst<String>(size, get<const String &>(field));
}

}
