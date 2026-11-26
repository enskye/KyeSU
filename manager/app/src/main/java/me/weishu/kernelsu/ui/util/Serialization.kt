package me.weishu.kernelsu.ui.util

import android.os.Parcel
import android.os.Parcelable
import kotlinx.parcelize.parcelableCreator
import kotlinx.serialization.KSerializer
import kotlinx.serialization.SerializationException
import kotlinx.serialization.builtins.ByteArraySerializer
import kotlinx.serialization.encoding.Decoder
import kotlinx.serialization.encoding.Encoder
import me.weishu.kernelsu.ui.screen.flash.FlashIt
import me.weishu.kernelsu.ui.screen.modulerepo.RepoModuleArg
import me.weishu.kernelsu.ui.screen.scriptrun.ScriptRunArg
import me.weishu.kernelsu.ui.viewmodel.TemplateViewModel

// Stable tags are part of the saved navigation format; do not renumber them.
object FlashItSerializer : BaseParcelableSerializer<FlashIt>(
    read = { parcel ->
        when (val tag = parcel.readInt()) {
            0 -> parcelableCreator<FlashIt.FlashBoot>().createFromParcel(parcel)
            1 -> parcelableCreator<FlashIt.DownloadBoot>().createFromParcel(parcel)
            2 -> parcelableCreator<FlashIt.FlashModules>().createFromParcel(parcel)
            3 -> FlashIt.FlashRestore
            4 -> FlashIt.FlashUninstall
            else -> throw SerializationException("Unknown FlashIt tag: $tag")
        }
    },
    write = { parcel, value ->
        when (value) {
            is FlashIt.FlashBoot -> {
                parcel.writeInt(0)
                value.writeToParcel(parcel, 0)
            }
            is FlashIt.DownloadBoot -> {
                parcel.writeInt(1)
                value.writeToParcel(parcel, 0)
            }
            is FlashIt.FlashModules -> {
                parcel.writeInt(2)
                value.writeToParcel(parcel, 0)
            }
            FlashIt.FlashRestore -> parcel.writeInt(3)
            FlashIt.FlashUninstall -> parcel.writeInt(4)
        }
    },
)

object RepoModuleArgSerializer : BaseParcelableSerializer<RepoModuleArg>(
    read = { parcelableCreator<RepoModuleArg>().createFromParcel(it) },
)
object ScriptRunArgSerializer : BaseParcelableSerializer<ScriptRunArg>(
    read = { parcelableCreator<ScriptRunArg>().createFromParcel(it) },
)
object TemplateInfoSerializer : BaseParcelableSerializer<TemplateViewModel.TemplateInfo>(
    read = { parcelableCreator<TemplateViewModel.TemplateInfo>().createFromParcel(it) },
)

open class BaseParcelableSerializer<T : Parcelable>(
    private val read: (Parcel) -> T,
    private val write: (Parcel, T) -> Unit = { parcel, value -> value.writeToParcel(parcel, 0) },
) : KSerializer<T> {

    private val delegate = ByteArraySerializer()
    override val descriptor = delegate.descriptor

    override fun serialize(encoder: Encoder, value: T) {
        val parcel = Parcel.obtain()
        try {
            write(parcel, value)
            val bytes = parcel.marshall()
            encoder.encodeSerializableValue(delegate, bytes)
        } finally {
            parcel.recycle()
        }
    }

    override fun deserialize(decoder: Decoder): T {
        val bytes = decoder.decodeSerializableValue(delegate)
        val parcel = Parcel.obtain()
        try {
            parcel.unmarshall(bytes, 0, bytes.size)
            parcel.setDataPosition(0)
            return read(parcel)
        } finally {
            parcel.recycle()
        }
    }
}
